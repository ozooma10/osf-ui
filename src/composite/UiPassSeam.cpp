#include "composite/UiPassSeam.h"

#include "composite/D3D12Compositor.h"  // RecordSeamOverlayDraw (real-overlay seam draw)
#include "composite/EngineD3D12.h"
#include "core/Log.h"
#include "platform/WindowsPlatform.h"

// GDI-free Win32/D3D12 so the ERROR macro never collides with REX::ERROR.
#define WIN32_LEAN_AND_MEAN
#define NOGDI
#define NOMINMAX
#include <Windows.h>

#include <d3d12.h>

#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstring>

namespace OSFUI::UiPassSeam
{
	namespace
	{
		// AddrLib IDs, proven on 1.16.244. Canonical record with disassembly
		// evidence: OSF RE context module `rendering.ui_pass` (2026-07-21).
		constexpr std::uint64_t kVtblScaleformBegin = 497423;      // anon-ns ScaleformBegin pass vtable
		constexpr std::uint64_t kVtblScaleformEnd = 497425;        // anon-ns ScaleformEnd pass vtable
		constexpr std::uint64_t kVtblScaleformComposite = 497272;  // ScaleformCompositeRenderPass vtable
		constexpr std::uint64_t kIdBeginExecute = 145955;          // ScaleformBegin::ExecuteRenderPass
		constexpr std::uint64_t kIdEndExecute = 145956;            // ScaleformEnd::ExecuteRenderPass
		constexpr std::uint64_t kIdCompositeExecute = 145827;      // ScaleformCompositeRenderPass::ExecuteRenderPass
		constexpr std::uint64_t kIdGraphCtxGetCmdCtx = 144161;     // GraphContext -> engine command context getter

		// RenderPass vtable slot 7 = ExecuteRenderPass(this, GraphContext*, IOHandles*).
		constexpr std::size_t kExecuteSlot = 7;

		constexpr std::uint32_t kMaxDetailLogs = 8;  // per site: plain call logs

		using ExecuteFn = void* (*)(void*, void*, void*, void*);
		using GetCmdCtxFn = void* (*)(void*);

		std::atomic<std::uintptr_t> g_origBegin{ 0 };
		std::atomic<std::uintptr_t> g_origEnd{ 0 };
		std::atomic<std::uintptr_t> g_origComposite{ 0 };
		std::atomic<bool> g_installed{ false };
		std::atomic<bool> g_installOk{ false };
		std::atomic<bool> g_probeEnabled{ false };

		struct SiteState
		{
			std::atomic<std::uint64_t> count{ 0 };
		};
		SiteState g_end;
		SiteState g_composite;

		// ---------------------------------------------------------------- SEH
		// GetEngineCmdCtx calls an engine getter through a pointer we only
		// BELIEVE is a live GraphContext; the call sits behind SEH so a wrong
		// guess logs nothing instead of crashing a render worker. This helper
		// holds no C++ objects (C2712: __try cannot share a frame with
		// unwinding).

		[[nodiscard]] void* CallGetterSeh(GetCmdCtxFn a_fn, void* a_arg) noexcept
		{
			__try {
				return a_fn(a_arg);
			} __except (EXCEPTION_EXECUTE_HANDLER) {
				return nullptr;
			}
		}

		[[nodiscard]] const char* FormatName(const DXGI_FORMAT a_format)
		{
			switch (a_format) {
				case DXGI_FORMAT_R8G8B8A8_UNORM:       return "R8G8B8A8_UNORM";
				case DXGI_FORMAT_R8G8B8A8_UNORM_SRGB:  return "R8G8B8A8_UNORM_SRGB";
				case DXGI_FORMAT_B8G8R8A8_UNORM:       return "B8G8R8A8_UNORM";
				case DXGI_FORMAT_B8G8R8A8_UNORM_SRGB:  return "B8G8R8A8_UNORM_SRGB";
				case DXGI_FORMAT_R10G10B10A2_UNORM:    return "R10G10B10A2_UNORM";
				case DXGI_FORMAT_R11G11B10_FLOAT:      return "R11G11B10_FLOAT";
				case DXGI_FORMAT_R16G16B16A16_FLOAT:   return "R16G16B16A16_FLOAT";
				default:                               return "other";
			}
		}

		// ------------------------------------------------------------- engine
		[[nodiscard]] void* GetEngineCmdCtx(void* a_graphCtx)
		{
			if (!a_graphCtx) {
				return nullptr;
			}
			static const REL::Relocation<GetCmdCtxFn> getter{ REL::ID(kIdGraphCtxGetCmdCtx) };
			return CallGetterSeh(getter.get(), a_graphCtx);
		}

		// ------------------------------------------------- D3D12 call capture
		// The blind scans found no D3D12 interfaces reachable from the engine
		// command context (first run: zero hits at 0x200/1-hop), so instead of
		// guessing offsets, catch the engine in the act: hook the process-wide
		// ID3D12GraphicsCommandList vtable (obtained from a throwaway list
		// created on the game's own device — same trick as the Present hook's
		// throwaway swapchain) and log, ONLY while ScaleformComposite's
		// original Execute runs on the bracket thread, which list sets render
		// targets and which resources get barrier-transitioned. That yields
		// the live list pointer (searched back through the ctx object to
		// derive its offset) and the composite target's format via GetDesc.
		//
		// Slot indices are fixed COM ABI, straight from the d3d12.h C vtable
		// (ID3D12GraphicsCommandListVtbl): 26 = ResourceBarrier,
		// 46 = OMSetRenderTargets. Trust but verify: after hooking, each slot
		// is self-tested by calling it on our own throwaway list and checking
		// the thunk observed the sentinel — a wrong index unhooks itself.
		constexpr std::size_t kSlotResourceBarrier = 26;
		constexpr std::size_t kSlotSetDescriptorHeaps = 28;
		constexpr std::size_t kSlotOMSetRenderTargets = 46;


		// Whole-UI-frame capture WINDOW (phase 1b): armed at ScaleformBegin
		// entry, disarmed after ScaleformComposite returns. The RE probe proved
		// the whole Begin -> movie renders -> End -> [graph glue / Frame
		// Interpolation] -> Composite sequence stays on ONE worker thread per
		// frame, so a tid-keyed window catches every OMSetRenderTargets and
		// barrier the engine records for the UI section — including
		// ScaleformCompositeBuffer's clear/transitions, which never appeared in
		// the old composite-only bracket. Phase-marker lines (Begin/End/
		// Composite enter+exit) segment the capture temporally, answering which
		// side of the buffer's RT->SRV transition each pass sits on.
		constexpr std::uint32_t kMaxCaptureWindows = 4;   // full UI windows to log
		constexpr int           kWindowLineBudget = 48;   // OM+barrier lines per window

		using OMSetRenderTargetsFn = void(STDMETHODCALLTYPE*)(
			ID3D12GraphicsCommandList*, UINT, const D3D12_CPU_DESCRIPTOR_HANDLE*, BOOL,
			const D3D12_CPU_DESCRIPTOR_HANDLE*);
		using ResourceBarrierFn = void(STDMETHODCALLTYPE*)(
			ID3D12GraphicsCommandList*, UINT, const D3D12_RESOURCE_BARRIER*);
		using SetDescriptorHeapsFn = void(STDMETHODCALLTYPE*)(
			ID3D12GraphicsCommandList*, UINT, ID3D12DescriptorHeap* const*);

		std::atomic<OMSetRenderTargetsFn> g_origOMSetRenderTargets{ nullptr };
		std::atomic<ResourceBarrierFn> g_origResourceBarrier{ nullptr };
		std::atomic<SetDescriptorHeapsFn> g_origSetDescriptorHeaps{ nullptr };

		// The engine's live descriptor-heap binding, tracked per thread: the
		// real-overlay seam draw binds the compositor's shader-visible heap on
		// the engine's list and must restore the engine's afterwards — the
		// engine's abstraction layer caches bound state and may legally skip a
		// "redundant" rebind downstream.
		thread_local ID3D12GraphicsCommandList* tl_heapList = nullptr;
		thread_local ID3D12DescriptorHeap* tl_heaps[2] = {};
		thread_local UINT tl_heapCount = 0;
		std::atomic<std::uint32_t> g_captureTid{ 0 };       // window: UI worker tid, else 0
		std::atomic<std::uintptr_t> g_capturedList{ 0 };    // last list seen inside the window
		std::atomic<std::uint32_t> g_captureWindows{ 0 };   // windows completed so far
		std::atomic<int> g_windowLines{ kWindowLineBudget };  // per-window log budget
		std::atomic<int> g_captureState{ 0 };               // 0 untried, 1 ready, -1 failed

		// One budgeted log line inside the active window (present thread only
		// checks tid first — cheap two loads on the common path).
		[[nodiscard]] bool WindowLogSlot()
		{
			return g_windowLines.fetch_sub(1, std::memory_order_relaxed) > 0;
		}

		// Seam-draw state shared with the barrier thunk (phase 2; the draw
		// machinery itself is further down). Armed at End-exit, cleared at
		// Composite-enter: inside that span, the first RENDER_TARGET ->
		// shader-resource transition this thread records IS this frame's
		// ScaleformCompositeBuffer hand-off, and the draw happens right there
		// (before the barrier is forwarded) — the only point with the CURRENT
		// frame's resource, still in RT state, on the current recording list.
		// v1 latched the pointer and drew NEXT frame from the End thunk; load
		// transitions churn the transient buffer pool (three different buffers
		// in 2.3 s, 2026-07-21) and a one-frame-stale render-target write into
		// re-aliased pool memory GPU-faulted the game. Never target a graph
		// transient across frames.
		// Hand-off draws remaining in the current End glue region. With FG
		// active the glue right after End records TWO UI hand-offs back to
		// back (gameplay capture 2026-07-21): the composite-pass input
		// (RT -> pixel-SRV, feeds REAL frames) and the FG UI-input source
		// (RT -> COPY_SOURCE, copied to the texture FI's compute composites
		// onto GENERATED frames). Drawing into both kills the FG 2x flicker;
		// both sit in glue, not inside another pass's stream (the v4 crash).
		thread_local int tl_handoffDrawsLeft = 0;
		// After the first hand-off draw, the second legitimate hand-off sits
		// within the next couple of barrier calls (same glue batch — capture
		// 2026-07-21). Expire the remaining slot after a few calls so it can
		// never fire deep in the window inside another pass's stream.
		thread_local int tl_callsAfterFirstDraw = -1;  // -1 = no draw yet this region
		// a_fgTarget identifies the RT -> COPY_SOURCE hand-off.
		// a_regionFirst: first draw of this End
		// region — the compositor promotes its ring serial only then, so both
		// targets of one frame sample the SAME overlay frame.
		void RecordSeamDrawAtHandoff(ID3D12GraphicsCommandList* a_list, ID3D12Resource* a_buffer,
			bool a_fgTarget, bool a_regionFirst);
		ID3D12GraphicsCommandList* g_selfTestList = nullptr;  // non-null only during self-test
		std::atomic<bool> g_selfTestOMSeen{ false };
		std::atomic<bool> g_selfTestBarrierSeen{ false };
		std::atomic<bool> g_selfTestHeapsSeen{ false };

		void STDMETHODCALLTYPE SetDescriptorHeapsThunk(
			ID3D12GraphicsCommandList* a_self,
			const UINT a_num,
			ID3D12DescriptorHeap* const* a_heaps)
		{
			if (a_self == g_selfTestList) {
				g_selfTestHeapsSeen.store(true, std::memory_order_relaxed);
			} else {
				tl_heapList = a_self;
				tl_heapCount = a_num < 2u ? a_num : 2u;
				for (UINT i = 0; i < tl_heapCount; ++i) {
					tl_heaps[i] = a_heaps ? a_heaps[i] : nullptr;
				}
			}
			if (const auto original = g_origSetDescriptorHeaps.load(std::memory_order_relaxed)) {
				original(a_self, a_num, a_heaps);
			}
		}

		void STDMETHODCALLTYPE OMSetRenderTargetsThunk(
			ID3D12GraphicsCommandList* a_self,
			const UINT a_numRTs,
			const D3D12_CPU_DESCRIPTOR_HANDLE* a_rts,
			const BOOL a_singleRange,
			const D3D12_CPU_DESCRIPTOR_HANDLE* a_dsv)
		{
			if (a_self == g_selfTestList) {
				g_selfTestOMSeen.store(true, std::memory_order_relaxed);
			} else if (g_captureTid.load(std::memory_order_relaxed) == ::GetCurrentThreadId() && WindowLogSlot()) {
				g_capturedList.store(reinterpret_cast<std::uintptr_t>(a_self), std::memory_order_relaxed);
				REX::INFO("[UiPassSeam] capture OMSetRenderTargets list=0x{:X} numRTs={} rtv0=0x{:X} dsv={}",
					reinterpret_cast<std::uintptr_t>(a_self), a_numRTs,
					(a_rts && a_numRTs > 0) ? a_rts[0].ptr : 0,
					a_dsv ? "yes" : "no");
			}
			if (const auto original = g_origOMSetRenderTargets.load(std::memory_order_relaxed)) {
				original(a_self, a_numRTs, a_rts, a_singleRange, a_dsv);
			}
		}

		void STDMETHODCALLTYPE ResourceBarrierThunk(
			ID3D12GraphicsCommandList* a_self,
			const UINT a_numBarriers,
			const D3D12_RESOURCE_BARRIER* a_barriers)
		{
			// THE SEAM (see tl_handoffDrawsLeft): draw into this frame's UI
			// buffers right before their hand-off barriers are recorded.
			// Matching is strict on all axes because the transition alone does
			// NOT identify a UI buffer — load-transition graphs interleave
			// other nodes' passes on this worker, and drawing into a foreign
			// RT (wrong format for our R8G8B8A8_UNORM view) or onto a
			// non-DIRECT list faults the GPU. Accepted hand-offs: RT ->
			// pixel-SRV (composite input, real frames) and RT -> COPY_SOURCE
			// (FG UI-input source, generated frames). At most two draws per
			// End region — drawing at arbitrary later matches (v4) injected
			// state into the middle of other passes' streams, whose recording
			// then used our clobbered state via the engine's cached-state
			// abstraction layer and null-derefed the driver.
			if (tl_handoffDrawsLeft > 0 && tl_callsAfterFirstDraw >= 0 && ++tl_callsAfterFirstDraw > 4) {
				tl_handoffDrawsLeft = 0;  // second hand-off would have appeared by now
				tl_callsAfterFirstDraw = -1;
			}
			if (tl_handoffDrawsLeft > 0 && a_barriers) {
				for (UINT i = 0; i < a_numBarriers && tl_handoffDrawsLeft > 0; ++i) {
					const auto& barrier = a_barriers[i];
					if (barrier.Type != D3D12_RESOURCE_BARRIER_TYPE_TRANSITION || !barrier.Transition.pResource ||
						barrier.Transition.StateBefore != D3D12_RESOURCE_STATE_RENDER_TARGET ||
						!(barrier.Transition.StateAfter &
							(D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE | D3D12_RESOURCE_STATE_COPY_SOURCE))) {
						continue;
					}
					const auto desc = barrier.Transition.pResource->GetDesc();
					if (desc.Format != DXGI_FORMAT_R8G8B8A8_TYPELESS ||
						desc.Dimension != D3D12_RESOURCE_DIMENSION_TEXTURE2D ||
						desc.SampleDesc.Count != 1 ||
						desc.Width < 256 || desc.Height < 256) {
						continue;
					}
					if (a_self->GetType() != D3D12_COMMAND_LIST_TYPE_DIRECT) {
						continue;
					}
					const bool fgTarget =
						(barrier.Transition.StateAfter & D3D12_RESOURCE_STATE_COPY_SOURCE) != 0 &&
						(barrier.Transition.StateAfter & D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE) == 0;
					const bool regionFirst = tl_handoffDrawsLeft == 2;  // pins both draws to one ring serial
					--tl_handoffDrawsLeft;
					if (tl_callsAfterFirstDraw < 0) {
						tl_callsAfterFirstDraw = 0;  // start the expiry clock at the first draw
					}
					RecordSeamDrawAtHandoff(
						a_self, barrier.Transition.pResource, fgTarget, regionFirst);
				}
			}

			if (a_self == g_selfTestList) {
				g_selfTestBarrierSeen.store(true, std::memory_order_relaxed);
			} else if (g_captureTid.load(std::memory_order_relaxed) == ::GetCurrentThreadId()) {
				g_capturedList.store(reinterpret_cast<std::uintptr_t>(a_self), std::memory_order_relaxed);
				const auto count = a_barriers ? (a_numBarriers < 8u ? a_numBarriers : 8u) : 0u;
				for (UINT i = 0; i < count; ++i) {
					const auto& barrier = a_barriers[i];
					if (barrier.Type != D3D12_RESOURCE_BARRIER_TYPE_TRANSITION || !barrier.Transition.pResource) {
						continue;
					}
					if (!WindowLogSlot()) {
						break;
					}
					const auto desc = barrier.Transition.pResource->GetDesc();
					REX::INFO("[UiPassSeam] capture barrier list=0x{:X} res=0x{:X} {}x{} fmt={} ({}) states 0x{:X}->0x{:X}",
						reinterpret_cast<std::uintptr_t>(a_self),
						reinterpret_cast<std::uintptr_t>(barrier.Transition.pResource),
						static_cast<std::uint64_t>(desc.Width), desc.Height,
						static_cast<int>(desc.Format), FormatName(desc.Format),
						static_cast<std::uint32_t>(barrier.Transition.StateBefore),
						static_cast<std::uint32_t>(barrier.Transition.StateAfter));
				}
			}
			if (const auto original = g_origResourceBarrier.load(std::memory_order_relaxed)) {
				original(a_self, a_numBarriers, a_barriers);
			}
		}

		[[nodiscard]] void* PatchSlot(void** a_vtbl, const std::size_t a_slot, void* a_thunk)
		{
			DWORD oldProtect = 0;
			if (!::VirtualProtect(&a_vtbl[a_slot], sizeof(void*), PAGE_READWRITE, &oldProtect)) {
				return nullptr;
			}
			void* original = a_vtbl[a_slot];
			a_vtbl[a_slot] = a_thunk;
			::VirtualProtect(&a_vtbl[a_slot], sizeof(void*), oldProtect, &oldProtect);
			return original;
		}

		// Lazy, once. Creates a throwaway DIRECT list on the game's device to
		// reach the shared d3d12.dll vtable, hooks the two slots, and proves
		// the slot indices by calling both methods on the throwaway list
		// (OMSetRenderTargets with zero targets and a global-UAV barrier are
		// both legal no-ops). A failed self-test restores the slots.
		void EnsureCaptureInstalled()
		{
			int expected = 0;
			if (!g_captureState.compare_exchange_strong(expected, -1, std::memory_order_acq_rel)) {
				return;  // already tried (state is 1 or -1); never retry
			}

			const auto engine = LocateEngineD3D12();
			if (!engine) {
				REX::WARN("[UiPassSeam] capture: engine D3D12 device not reachable; skipping call capture");
				return;
			}

			ID3D12CommandAllocator* allocator = nullptr;
			ID3D12GraphicsCommandList* list = nullptr;
			auto* device = reinterpret_cast<ID3D12Device*>(engine.device);
			const bool created =
				SUCCEEDED(device->CreateCommandAllocator(
					D3D12_COMMAND_LIST_TYPE_DIRECT, __uuidof(ID3D12CommandAllocator),
					reinterpret_cast<void**>(&allocator))) &&
				SUCCEEDED(device->CreateCommandList(
					0, D3D12_COMMAND_LIST_TYPE_DIRECT, allocator, nullptr,
					__uuidof(ID3D12GraphicsCommandList), reinterpret_cast<void**>(&list)));

			if (created) {
				const bool probe = g_probeEnabled.load(std::memory_order_relaxed);
				auto** vtbl = *reinterpret_cast<void***>(list);
				g_selfTestList = list;
				// Publish each forward target into its atomic BEFORE its thunk becomes
				// reachable on the shared d3d12.dll vtable (release store), so a
				// concurrent render worker routing through a just-installed thunk can
				// never load a null original and drop the engine's D3D12 call. Barrier
				// + SetDescriptorHeaps are load-bearing for the seam draw (hand-off
				// match + engine heap restore); OMSetRenderTargets only feeds the
				// uiPassProbe capture log, so hook it only under the probe instead of
				// leaving a thunk on the process-global vtable running on every
				// OMSetRenderTargets call in the game for an inert release diagnostic.
				const auto origBarrier = reinterpret_cast<ResourceBarrierFn>(vtbl[kSlotResourceBarrier]);
				const auto origHeaps = reinterpret_cast<SetDescriptorHeapsFn>(vtbl[kSlotSetDescriptorHeaps]);
				g_origResourceBarrier.store(origBarrier, std::memory_order_release);
				g_origSetDescriptorHeaps.store(origHeaps, std::memory_order_release);
				const bool patchedBarrier =
					PatchSlot(vtbl, kSlotResourceBarrier, reinterpret_cast<void*>(&ResourceBarrierThunk)) != nullptr;
				const bool patchedHeaps =
					PatchSlot(vtbl, kSlotSetDescriptorHeaps, reinterpret_cast<void*>(&SetDescriptorHeapsThunk)) != nullptr;
				OMSetRenderTargetsFn origOM = nullptr;
				bool patchedOM = false;
				if (probe) {
					origOM = reinterpret_cast<OMSetRenderTargetsFn>(vtbl[kSlotOMSetRenderTargets]);
					g_origOMSetRenderTargets.store(origOM, std::memory_order_release);
					patchedOM =
						PatchSlot(vtbl, kSlotOMSetRenderTargets, reinterpret_cast<void*>(&OMSetRenderTargetsThunk)) != nullptr;
				}

				// Prove the slot indices by calling each hooked method on the throwaway
				// list (all legal no-ops) and checking the thunk saw the sentinel.
				// Barrier + heaps are mandatory; OM only when probe-hooked.
				const bool patched = patchedBarrier && patchedHeaps && (!probe || patchedOM);
				if (patched) {
					D3D12_RESOURCE_BARRIER uav{};
					uav.Type = D3D12_RESOURCE_BARRIER_TYPE_UAV;
					uav.UAV.pResource = nullptr;  // global UAV barrier: legal on any list
					list->ResourceBarrier(1, &uav);
					list->SetDescriptorHeaps(0, nullptr);  // clearing heaps: legal no-op
					if (probe) {
						list->OMSetRenderTargets(0, nullptr, FALSE, nullptr);
					}
				}

				const bool barrierOk = g_selfTestBarrierSeen.load(std::memory_order_relaxed);
				const bool heapsOk = g_selfTestHeapsSeen.load(std::memory_order_relaxed);
				const bool omSeen = g_selfTestOMSeen.load(std::memory_order_relaxed);
				if (patched && barrierOk && heapsOk && (!probe || omSeen)) {
					g_selfTestList = nullptr;
					g_captureState.store(1, std::memory_order_release);
					REX::INFO("[UiPassSeam] capture armed: ID3D12GraphicsCommandList vtable slots {} (barrier) / {} "
							  "(SetDescriptorHeaps){} hooked and self-tested",
						kSlotResourceBarrier, kSlotSetDescriptorHeaps,
						probe ? " / OMSetRenderTargets" : "");
				} else {
					// Wrong slot layout or protect failure: undo what we did.
					if (patchedOM) {
						(void)PatchSlot(vtbl, kSlotOMSetRenderTargets, reinterpret_cast<void*>(origOM));
					}
					if (patchedBarrier) {
						(void)PatchSlot(vtbl, kSlotResourceBarrier, reinterpret_cast<void*>(origBarrier));
					}
					if (patchedHeaps) {
						(void)PatchSlot(vtbl, kSlotSetDescriptorHeaps, reinterpret_cast<void*>(origHeaps));
					}
					g_origOMSetRenderTargets.store(nullptr, std::memory_order_relaxed);
					g_origResourceBarrier.store(nullptr, std::memory_order_relaxed);
					g_origSetDescriptorHeaps.store(nullptr, std::memory_order_relaxed);
					g_selfTestList = nullptr;
					REX::WARN("[UiPassSeam] capture self-test FAILED (patch b/h/om={}/{}/{} seen b/h/om={}/{}/{}); vtable restored, call capture disabled",
						patchedBarrier, patchedHeaps, patchedOM, barrierOk, heapsOk, omSeen);
				}
				list->Close();
			} else {
				REX::WARN("[UiPassSeam] capture: throwaway command list creation failed; skipping call capture");
			}

			if (list) {
				list->Release();
			}
			if (allocator) {
				allocator->Release();
			}
			engine.directQueue->Release();
			engine.device->Release();
		}

		// After a captured window: where does the engine ctx keep that list?
		void ReportListOffset(const char* a_tag, const std::uintptr_t a_object, const std::uintptr_t a_list)
		{
			if (!a_object) {
				return;
			}
			bool found = false;
			for (std::size_t offset = 0; offset < 0x1000; offset += sizeof(std::uintptr_t)) {
				std::uintptr_t value = 0;
				if (Platform::SafeReadPointer(a_object + offset, value) && value == a_list) {
					REX::INFO("[UiPassSeam] capture: {}+0x{:X} holds the recording ID3D12GraphicsCommandList", a_tag, offset);
					found = true;
				}
			}
			if (!found) {
				REX::INFO("[UiPassSeam] capture: list 0x{:X} not stored in {}'s first 0x1000 bytes", a_list, a_tag);
			}
		}

		// ------------------------------------------------------ seam draw state
		std::atomic<bool> g_drawEnabled{ false };
		std::atomic<std::uint64_t> g_seamDraws{ 0 };
		std::atomic<std::uintptr_t> g_lastDrawBuffer{ 0 };   // churn diagnostics only
		std::atomic<std::uint32_t> g_bufferChangeLogs{ 0 };  // bounded (loads churn hard)

		// Inside the hand-off barrier: a_buffer is THIS frame's UI buffer,
		// still in RENDER_TARGET state (the transition is forwarded after we
		// return), and a_list is the recording list it was drawn on.
		void RecordSeamDrawAtHandoff(ID3D12GraphicsCommandList* a_list, ID3D12Resource* a_buffer,
			const bool a_fgTarget, const bool a_regionFirst)
		{
			if (!g_drawEnabled.load(std::memory_order_relaxed) || !a_list || !a_buffer) {
				return;
			}

			// Snapshot the engine's descriptor-heap binding before the
			// compositor binds its own shader-visible heap.
			ID3D12DescriptorHeap* engineHeaps[2]{ tl_heaps[0], tl_heaps[1] };
			const UINT engineHeapCount = tl_heapCount;
			const bool heapKnown = engineHeapCount > 0 && tl_heapList == a_list;

			if (!RecordSeamOverlayDraw(a_list, a_buffer, a_fgTarget, a_regionFirst)) {
				return;
			}
			if (heapKnown) {
				if (const auto original = g_origSetDescriptorHeaps.load(std::memory_order_relaxed)) {
					original(a_list, engineHeapCount, engineHeaps);
				}
			}
			// Keep the draw counter live for the probe-side window-arm log, but
			// gate the diagnostics: the seam draw runs on a render worker in the
			// default (uiPassDraw) build, so they stay silent unless uiPassProbe.
			const auto draws = g_seamDraws.fetch_add(1, std::memory_order_relaxed) + 1;
			if (g_probeEnabled.load(std::memory_order_relaxed)) {
				if (draws == 1) {
					REX::INFO("[UiPassSeam] draw: FIRST SEAM DRAW recorded at the hand-off (buffer 0x{:X}, list 0x{:X})",
						reinterpret_cast<std::uintptr_t>(a_buffer),
						reinterpret_cast<std::uintptr_t>(a_list));
				}
				// Churn diagnostics: buffer pointer changes mark graph rebuilds
				// (loads); the periodic count correlates a crash time with whether
				// draws were still being recorded.
				const auto prev = g_lastDrawBuffer.exchange(reinterpret_cast<std::uintptr_t>(a_buffer), std::memory_order_relaxed);
				if (prev != reinterpret_cast<std::uintptr_t>(a_buffer) && prev != 0 &&
					g_bufferChangeLogs.fetch_add(1, std::memory_order_relaxed) < 12) {
					REX::INFO("[UiPassSeam] draw: hand-off buffer changed 0x{:X} -> 0x{:X} (draw #{})",
						prev, reinterpret_cast<std::uintptr_t>(a_buffer), draws);
				}
				if (draws % 18000 == 0) {  // liveness only, ~every 1-2 min; logging here runs on a render worker
					REX::INFO("[UiPassSeam] draw: {} seam draws recorded", draws);
				}
			}
		}

		// ------------------------------------------------------------- thunks
		void* BeginThunk(void* a_this, void* a_ctx, void* a_io, void* a_r9)
		{
			const bool probe = g_probeEnabled.load(std::memory_order_relaxed);

			// Arm the capture window PRE-original: ScaleformBegin's own
			// recording (the buffer's clear/acquire, if anywhere) must land
			// inside it. A tid re-match re-arms a window left open by a frame
			// whose composite never ran.
			EnsureCaptureInstalled();
			const auto tid = ::GetCurrentThreadId();
			bool window = false;
			// Only the first kMaxCaptureWindows fire (session start / main
			// menu). Periodic gameplay re-arms are RETIRED: each window bursts
			// dozens of synchronous log lines on a render worker mid-frame —
			// a visible hitch every ~5000 draws (2026-07-21) — and the FG
			// UI-input pipeline they existed to capture is now decoded (see
			// tl_handoffDrawsLeft). Re-add a bounded re-arm only when a new
			// unknown needs gameplay-state capture.
			const bool allowWindow = probe && g_captureWindows.load(std::memory_order_relaxed) < kMaxCaptureWindows;
			if (allowWindow) {
				std::uint32_t expected = 0;
				if (g_captureTid.compare_exchange_strong(expected, tid, std::memory_order_acq_rel) ||
					expected == tid) {
					window = true;
					g_capturedList.store(0, std::memory_order_relaxed);
					g_windowLines.store(kWindowLineBudget, std::memory_order_relaxed);
					REX::INFO("[UiPassSeam] window {} armed: Begin enter tid={} (seam draws so far: {})",
						g_captureWindows.load(std::memory_order_relaxed) + 1, tid,
						g_seamDraws.load(std::memory_order_relaxed));
				}
			}

			const auto original = reinterpret_cast<ExecuteFn>(g_origBegin.load(std::memory_order_relaxed));
			void* result = original ? original(a_this, a_ctx, a_io, a_r9) : nullptr;

			if (window) {
				REX::INFO("[UiPassSeam] window: Begin exit");
			}
			return result;
		}

		void* EndThunk(void* a_this, void* a_ctx, void* a_io, void* a_r9)
		{
			const bool probe = g_probeEnabled.load(std::memory_order_relaxed);
			const auto count = probe ? g_end.count.fetch_add(1, std::memory_order_relaxed) + 1 : 0;
			const bool window = probe && g_captureTid.load(std::memory_order_relaxed) == ::GetCurrentThreadId();
			if (window) {
				REX::INFO("[UiPassSeam] window: End enter");
			}
			if (probe && count <= kMaxDetailLogs) {
				REX::INFO("[UiPassSeam] End[{}] tid={} this=0x{:X} ctx=0x{:X} io=0x{:X} cmdCtx=0x{:X}",
					count, ::GetCurrentThreadId(),
					reinterpret_cast<std::uintptr_t>(a_this), reinterpret_cast<std::uintptr_t>(a_ctx),
					reinterpret_cast<std::uintptr_t>(a_io),
					reinterpret_cast<std::uintptr_t>(GetEngineCmdCtx(a_ctx)));
			}

			const auto original = reinterpret_cast<ExecuteFn>(g_origEnd.load(std::memory_order_relaxed));
			void* result = original ? original(a_this, a_ctx, a_io, a_r9) : nullptr;

			// Arm the hand-off watch: every movie has rendered into the UI
			// buffers, and their hand-off transitions land in the glue after
			// this return (capture 2026-07-21). The barrier thunk draws right
			// there — the only spot with the CURRENT frame's buffers (the
			// transient pool churns during loads; a stale pointer GPU-faulted
			// the game). Two draws: composite input + FG UI-input source.
			tl_handoffDrawsLeft = 2;
			tl_callsAfterFirstDraw = -1;

			if (window) {
				REX::INFO("[UiPassSeam] window: End exit");
			}
			return result;
		}

		void* CompositeThunk(void* a_this, void* a_ctx, void* a_io, void* a_r9)
		{
			tl_handoffDrawsLeft = 0;  // hand-off glue region over for this frame
			tl_callsAfterFirstDraw = -1;
			const bool probe = g_probeEnabled.load(std::memory_order_relaxed);
			const auto count = probe ? g_composite.count.fetch_add(1, std::memory_order_relaxed) + 1 : 0;
			const bool window = probe && g_captureTid.load(std::memory_order_relaxed) == ::GetCurrentThreadId();
			if (window) {
				REX::INFO("[UiPassSeam] window: Composite enter");
			}

			// Pre-original probe of the state a draw would see here.
			std::uintptr_t cmdCtx = 0;
			if (probe && count <= kMaxDetailLogs) {
				cmdCtx = reinterpret_cast<std::uintptr_t>(GetEngineCmdCtx(a_ctx));
				REX::INFO("[UiPassSeam] Composite[{}] tid={} this=0x{:X} ctx=0x{:X} io=0x{:X} cmdCtx=0x{:X}",
					count, ::GetCurrentThreadId(),
					reinterpret_cast<std::uintptr_t>(a_this), reinterpret_cast<std::uintptr_t>(a_ctx),
					reinterpret_cast<std::uintptr_t>(a_io), cmdCtx);

			}

			const auto original = reinterpret_cast<ExecuteFn>(g_origComposite.load(std::memory_order_relaxed));
			void* result = original ? original(a_this, a_ctx, a_io, a_r9) : nullptr;

			// Window close (the whole UI section of this frame is now
			// recorded): report the list location and release the window slot.
			if (window) {
				const auto windowNo = g_captureWindows.fetch_add(1, std::memory_order_relaxed) + 1;
				const auto list = g_capturedList.load(std::memory_order_relaxed);
				if (list) {
					if (!cmdCtx) {
						cmdCtx = reinterpret_cast<std::uintptr_t>(GetEngineCmdCtx(a_ctx));
					}
					ReportListOffset("cmdCtx", cmdCtx, list);
				} else {
					// No D3D12 calls seen: capture disabled, or the whole UI
					// section skipped (no live movies).
					REX::INFO("[UiPassSeam] window {}: no OMSetRenderTargets/barrier calls captured", windowNo);
				}
				const auto left = g_windowLines.load(std::memory_order_relaxed);
				REX::INFO("[UiPassSeam] window {} closed (Composite exit, {} log slots left)",
					windowNo, left > 0 ? left : 0);
				g_captureTid.store(0, std::memory_order_release);
			}
			return result;
		}

		// -------------------------------------------------------------- hooks
		// Fail-closed slot swap: only hook when slot 7 still holds the engine's
		// own implementation (both sides resolved through AddrLib, so the guard
		// survives game patches that shift addresses). A foreign value means a
		// game-patch layout change or another mod hooked first — leave it alone
		// and say so, rather than chain onto an unknown ABI.
		[[nodiscard]] std::uintptr_t HookExecuteSlot(
			const char* a_label,
			const std::uint64_t a_vtblId,
			const std::uint64_t a_implId,
			ExecuteFn a_thunk,
			std::atomic<std::uintptr_t>& a_orig)
		{
			const REL::Relocation<std::uintptr_t> vtbl{ REL::ID(a_vtblId) };
			const REL::Relocation<std::uintptr_t> expected{ REL::ID(a_implId) };
			const auto slotAddress = vtbl.address() + kExecuteSlot * sizeof(std::uintptr_t);

			std::uintptr_t current = 0;
			if (!Platform::SafeReadPointer(slotAddress, current)) {
				REX::WARN("[UiPassSeam] {}: vtable slot at 0x{:X} unreadable; not hooking", a_label, slotAddress);
				return 0;
			}
			if (current != expected.address()) {
				REX::WARN("[UiPassSeam] {}: slot 7 holds 0x{:X}, expected 0x{:X} (game patch or foreign hook); not hooking",
					a_label, current, expected.address());
				return 0;
			}

			auto** slot = reinterpret_cast<void**>(slotAddress);
			DWORD oldProtect = 0;
			if (!::VirtualProtect(slot, sizeof(void*), PAGE_READWRITE, &oldProtect)) {
				REX::WARN("[UiPassSeam] {}: VirtualProtect failed; not hooking", a_label);
				return 0;
			}
			// Publish the forward target before the thunk becomes reachable, so a
			// concurrent pass call never loads a null original and skips the pass.
			a_orig.store(current, std::memory_order_release);
			*slot = reinterpret_cast<void*>(a_thunk);
			::VirtualProtect(slot, sizeof(void*), oldProtect, &oldProtect);
			REX::INFO("[UiPassSeam] hooked {} slot 7 (vtbl 0x{:X}, original 0x{:X})", a_label, vtbl.address(), current);
			return current;
		}
	}

	bool Install(const bool a_draw, const bool a_probe)
	{
		if (g_installed.exchange(true, std::memory_order_relaxed)) {
			return g_installOk.load(std::memory_order_acquire);
		}

		g_probeEnabled.store(a_probe, std::memory_order_relaxed);
		const auto origBegin = HookExecuteSlot(
			"ScaleformBegin", kVtblScaleformBegin, kIdBeginExecute, &BeginThunk, g_origBegin);
		const auto origEnd = HookExecuteSlot(
			"ScaleformEnd", kVtblScaleformEnd, kIdEndExecute, &EndThunk, g_origEnd);
		const auto origComposite = HookExecuteSlot(
			"ScaleformComposite", kVtblScaleformComposite, kIdCompositeExecute, &CompositeThunk, g_origComposite);

		const bool ok = origBegin != 0 && origEnd != 0 && origComposite != 0;
		g_installOk.store(ok, std::memory_order_release);
		g_drawEnabled.store(ok && a_draw, std::memory_order_release);
		if (!ok) {
			REX::WARN("[UiPassSeam] hook set incomplete — seam draw disabled; "
					  "the legacy present path remains active");
		} else if (a_draw) {
			REX::INFO("[UiPassSeam] seam draw enabled: overlay records into Starfield's "
					  "transparent UI layer at the ScaleformEnd hand-off");
		}
		return ok;
	}
}
