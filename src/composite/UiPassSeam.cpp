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
#include <d3dcompiler.h>

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
		constexpr std::uint64_t kIdScaleformHAL = 937259;          // Scaleform::Render::CreationRenderer::HAL*

		// Pass instances resolve their frame-graph resources through this index
		// (24-bit) — logged raw as a fallback route to ScaleformCompositeBuffer
		// should the window capture below never see its transition barrier.
		constexpr std::uint64_t kPassResourceIndexOffset = 0xF0;

		// RenderPass vtable slot 7 = ExecuteRenderPass(this, GraphContext*, IOHandles*).
		constexpr std::size_t kExecuteSlot = 7;

		// Valid only inside a Scaleform pass Execute (bound by ScaleformBegin,
		// cleared before the bracket returns) — read there, never cached.
		constexpr std::uint64_t kHalRTOffset = 0x1410;   // UI render-target info (graph-transient stack ptr)
		constexpr std::uint64_t kHalCtxOffset = 0x1418;  // engine command context

		constexpr std::uint32_t kMaxDetailLogs = 8;  // per site: plain call logs
		constexpr std::uint32_t kMaxScans = 2;       // per site: deep pointer scans (verbose)

		using ExecuteFn = void* (*)(void*, void*, void*, void*);
		using GetCmdCtxFn = void* (*)(void*);

		std::atomic<std::uintptr_t> g_origBegin{ 0 };
		std::atomic<std::uintptr_t> g_origEnd{ 0 };
		std::atomic<std::uintptr_t> g_origComposite{ 0 };
		std::atomic<bool> g_installed{ false };

		struct SiteState
		{
			std::atomic<std::uint64_t> count{ 0 };
			std::atomic<std::uint32_t> scans{ 0 };
			std::atomic<std::uint32_t> instanceScans{ 0 };
		};
		SiteState g_begin;
		SiteState g_end;
		SiteState g_composite;

		// ---------------------------------------------------------------- SEH
		// The scans below make virtual calls on pointers we only BELIEVE are
		// live COM objects (gated on their vtable living inside d3d12.dll).
		// Each such call sits behind SEH so a wrong guess logs nothing instead
		// of crashing a render worker. These helpers hold no C++ objects
		// (C2712: __try cannot share a frame with unwinding).

		[[nodiscard]] bool TryQueryInterface(void* a_object, const IID& a_iid, void** a_out) noexcept
		{
			__try {
				return SUCCEEDED(static_cast<IUnknown*>(a_object)->QueryInterface(a_iid, a_out)) && *a_out;
			} __except (EXCEPTION_EXECUTE_HANDLER) {
				return false;
			}
		}

		[[nodiscard]] void* CallGetterSeh(GetCmdCtxFn a_fn, void* a_arg) noexcept
		{
			__try {
				return a_fn(a_arg);
			} __except (EXCEPTION_EXECUTE_HANDLER) {
				return nullptr;
			}
		}

		// ------------------------------------------------------------- module
		// A virtual call is only attempted when the candidate's vtable lies in
		// d3d12.dll — engine objects put a destructor at slot 0, so an
		// unguarded "QueryInterface" on one would destroy it.
		struct ModuleRange
		{
			std::uintptr_t base{ 0 };
			std::size_t size{ 0 };

			[[nodiscard]] bool Contains(const std::uintptr_t a_address) const
			{
				return base && a_address >= base && a_address < base + size;
			}
		};

		[[nodiscard]] ModuleRange LocateD3D12Module()
		{
			const auto module = ::GetModuleHandleW(L"d3d12.dll");
			if (!module) {
				return {};
			}
			const auto base = reinterpret_cast<std::uintptr_t>(module);
			const auto* dos = reinterpret_cast<const IMAGE_DOS_HEADER*>(base);
			const auto* nt = reinterpret_cast<const IMAGE_NT_HEADERS64*>(base + dos->e_lfanew);
			return { base, nt->OptionalHeader.SizeOfImage };
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

		// ------------------------------------------------------------- scanner
		// Walks an object's leading qwords looking for D3D12 interfaces, one
		// nesting level deep. Everything is SafeReadPointer'd (the blobs include
		// worker-stack memory) and log output is budgeted per scan.
		struct ScanBudget
		{
			std::uint32_t logs{ 40 };
			std::array<std::uintptr_t, 32> visited{};
			std::size_t visitedCount{ 0 };

			[[nodiscard]] bool MarkVisited(const std::uintptr_t a_ptr)
			{
				for (std::size_t i = 0; i < visitedCount; ++i) {
					if (visited[i] == a_ptr) {
						return false;
					}
				}
				if (visitedCount < visited.size()) {
					visited[visitedCount++] = a_ptr;
				}
				return true;
			}
		};

		// True when the candidate was a d3d12.dll COM object (logged either way
		// it classifies); false means "not COM — maybe recurse into it".
		bool ClassifyCandidate(
			const char* a_tag,
			const std::size_t a_offset,
			const std::uintptr_t a_candidate,
			const ModuleRange& a_d3d12,
			ScanBudget& a_budget)
		{
			std::uintptr_t vtbl = 0;
			if (!Platform::SafeReadPointer(a_candidate, vtbl) || !a_d3d12.Contains(vtbl)) {
				return false;
			}
			if (!a_budget.MarkVisited(a_candidate) || a_budget.logs == 0) {
				return true;
			}

			auto* object = reinterpret_cast<void*>(a_candidate);
			void* iface = nullptr;
			if (TryQueryInterface(object, __uuidof(ID3D12GraphicsCommandList), &iface)) {
				auto* list = static_cast<ID3D12GraphicsCommandList*>(iface);
				--a_budget.logs;
				REX::INFO("[UiPassSeam] {}+0x{:X} -> ID3D12GraphicsCommandList 0x{:X} (type={})",
					a_tag, a_offset, a_candidate, static_cast<int>(list->GetType()));
				list->Release();
			} else if (TryQueryInterface(object, __uuidof(ID3D12Resource), &iface)) {
				auto* resource = static_cast<ID3D12Resource*>(iface);
				const auto desc = resource->GetDesc();
				--a_budget.logs;
				REX::INFO("[UiPassSeam] {}+0x{:X} -> ID3D12Resource 0x{:X} dim={} {}x{} fmt={} ({}) flags=0x{:X}",
					a_tag, a_offset, a_candidate, static_cast<int>(desc.Dimension),
					static_cast<std::uint64_t>(desc.Width), desc.Height,
					static_cast<int>(desc.Format), FormatName(desc.Format),
					static_cast<std::uint32_t>(desc.Flags));
				resource->Release();
			} else if (TryQueryInterface(object, __uuidof(ID3D12CommandQueue), &iface)) {
				--a_budget.logs;
				REX::INFO("[UiPassSeam] {}+0x{:X} -> ID3D12CommandQueue 0x{:X}", a_tag, a_offset, a_candidate);
				static_cast<ID3D12CommandQueue*>(iface)->Release();
			} else if (TryQueryInterface(object, __uuidof(ID3D12CommandAllocator), &iface)) {
				--a_budget.logs;
				REX::INFO("[UiPassSeam] {}+0x{:X} -> ID3D12CommandAllocator 0x{:X}", a_tag, a_offset, a_candidate);
				static_cast<ID3D12CommandAllocator*>(iface)->Release();
			} else if (TryQueryInterface(object, __uuidof(ID3D12DescriptorHeap), &iface)) {
				--a_budget.logs;
				REX::INFO("[UiPassSeam] {}+0x{:X} -> ID3D12DescriptorHeap 0x{:X}", a_tag, a_offset, a_candidate);
				static_cast<ID3D12DescriptorHeap*>(iface)->Release();
			} else if (TryQueryInterface(object, __uuidof(ID3D12Object), &iface)) {
				--a_budget.logs;
				REX::INFO("[UiPassSeam] {}+0x{:X} -> ID3D12Object 0x{:X} (unclassified)", a_tag, a_offset, a_candidate);
				static_cast<ID3D12Object*>(iface)->Release();
			}
			return true;
		}

		// The blobs being scanned are raw worker-stack memory, so most qwords
		// are not pointers at all (counters, -1 sentinels, flag words). Filter
		// to the user-mode canonical range before spending a VirtualQuery on
		// them — and before arithmetic that could wrap (a -1 "pointer" plus a
		// size overflowed the pre-fix IsReadableRange into a false positive:
		// the probe's one field crash).
		[[nodiscard]] bool IsPlausiblePointer(const std::uintptr_t a_candidate)
		{
			return a_candidate >= 0x10000 && a_candidate < 0x0000'8000'0000'0000;
		}

		void ScanObject(
			const char* a_tag,
			const std::uintptr_t a_base,
			const std::size_t a_bytes,
			const int a_depth,
			const ModuleRange& a_d3d12,
			ScanBudget& a_budget)
		{
			char nestedTag[64];
			for (std::size_t offset = 0; offset < a_bytes; offset += sizeof(std::uintptr_t)) {
				if (a_budget.logs == 0) {
					return;
				}
				std::uintptr_t candidate = 0;
				if (!Platform::SafeReadPointer(a_base + offset, candidate) || !IsPlausiblePointer(candidate)) {
					continue;
				}
				if (ClassifyCandidate(a_tag, offset, candidate, a_d3d12, a_budget)) {
					continue;
				}
				// Not COM: one level of nesting covers engine wrapper structs
				// (e.g. an engine texture object holding the ID3D12Resource).
				if (a_depth > 0 && a_budget.MarkVisited(candidate) &&
					Platform::IsReadableRange(candidate, 0x100)) {
					std::snprintf(nestedTag, sizeof(nestedTag), "%s+0x%zX->", a_tag, offset);
					ScanObject(nestedTag, candidate, 0x100, a_depth - 1, a_d3d12, a_budget);
				}
			}
		}

		// ------------------------------------------------------------- engine
		[[nodiscard]] std::uintptr_t ReadHalField(const std::uint64_t a_offset)
		{
			static const REL::Relocation<std::uintptr_t*> halGlobal{ REL::ID(kIdScaleformHAL) };
			std::uintptr_t hal = 0;
			if (!Platform::SafeReadPointer(reinterpret_cast<std::uintptr_t>(halGlobal.get()), hal) || !hal) {
				return 0;
			}
			std::uintptr_t value = 0;
			(void)Platform::SafeReadPointer(hal + a_offset, value);
			return value;
		}

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

		// Draw the phase-2 debug triangle instead of the real overlay quad.
		// Kept for seam triage: flip to true to take the compositor (ring,
		// transport, heaps) out of the equation entirely.
		constexpr bool kSeamDebugTriangle = false;

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
		void RecordSeamDrawAtHandoff(ID3D12GraphicsCommandList* a_list, ID3D12Resource* a_buffer);
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
					--tl_handoffDrawsLeft;
					if (tl_callsAfterFirstDraw < 0) {
						tl_callsAfterFirstDraw = 0;  // start the expiry clock at the first draw
					}
					RecordSeamDrawAtHandoff(a_self, barrier.Transition.pResource);
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
				auto** vtbl = *reinterpret_cast<void***>(list);
				g_selfTestList = list;
				const auto origOM = PatchSlot(vtbl, kSlotOMSetRenderTargets, reinterpret_cast<void*>(&OMSetRenderTargetsThunk));
				const auto origBarrier = PatchSlot(vtbl, kSlotResourceBarrier, reinterpret_cast<void*>(&ResourceBarrierThunk));
				const auto origHeaps = PatchSlot(vtbl, kSlotSetDescriptorHeaps, reinterpret_cast<void*>(&SetDescriptorHeapsThunk));
				g_origOMSetRenderTargets.store(reinterpret_cast<OMSetRenderTargetsFn>(origOM), std::memory_order_relaxed);
				g_origResourceBarrier.store(reinterpret_cast<ResourceBarrierFn>(origBarrier), std::memory_order_relaxed);
				g_origSetDescriptorHeaps.store(reinterpret_cast<SetDescriptorHeapsFn>(origHeaps), std::memory_order_relaxed);

				if (origOM && origBarrier && origHeaps) {
					list->OMSetRenderTargets(0, nullptr, FALSE, nullptr);
					D3D12_RESOURCE_BARRIER uav{};
					uav.Type = D3D12_RESOURCE_BARRIER_TYPE_UAV;
					uav.UAV.pResource = nullptr;  // global UAV barrier: legal on any list
					list->ResourceBarrier(1, &uav);
					list->SetDescriptorHeaps(0, nullptr);  // clearing heaps: legal no-op
				}

				const bool omOk = g_selfTestOMSeen.load(std::memory_order_relaxed);
				const bool barrierOk = g_selfTestBarrierSeen.load(std::memory_order_relaxed);
				const bool heapsOk = g_selfTestHeapsSeen.load(std::memory_order_relaxed);
				if (origOM && origBarrier && origHeaps && omOk && barrierOk && heapsOk) {
					g_selfTestList = nullptr;
					g_captureState.store(1, std::memory_order_release);
					REX::INFO("[UiPassSeam] capture armed: ID3D12GraphicsCommandList vtable slots {} (barrier) / {} "
							  "(SetDescriptorHeaps) / {} (OMSetRenderTargets) hooked and self-tested",
						kSlotResourceBarrier, kSlotSetDescriptorHeaps, kSlotOMSetRenderTargets);
				} else {
					// Wrong slot layout or protect failure: undo what we did.
					if (origOM) {
						(void)PatchSlot(vtbl, kSlotOMSetRenderTargets, origOM);
					}
					if (origBarrier) {
						(void)PatchSlot(vtbl, kSlotResourceBarrier, origBarrier);
					}
					if (origHeaps) {
						(void)PatchSlot(vtbl, kSlotSetDescriptorHeaps, origHeaps);
					}
					g_origOMSetRenderTargets.store(nullptr, std::memory_order_relaxed);
					g_origResourceBarrier.store(nullptr, std::memory_order_relaxed);
					g_origSetDescriptorHeaps.store(nullptr, std::memory_order_relaxed);
					g_selfTestList = nullptr;
					REX::WARN("[UiPassSeam] capture self-test FAILED (patch={}/{}/{} seen={}/{}/{}); vtable restored, call capture disabled",
						origOM != nullptr, origBarrier != nullptr, origHeaps != nullptr, omOk, barrierOk, heapsOk);
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

		// Scan a pass instance once per site: cached D3D12 objects one hop deep
		// (ClassifyCandidate logs resources WITH GetDesc format/size — a cached
		// ScaleformCompositeBuffer pointer would settle phase 1b outright) plus
		// the raw graph-resource index at +0xF0 for the manager-table fallback.
		void ScanPassInstance(const char* a_tag, SiteState& a_site, void* a_this)
		{
			if (!a_this || a_site.instanceScans.load(std::memory_order_relaxed) >= kMaxScans) {
				return;
			}
			a_site.instanceScans.fetch_add(1, std::memory_order_relaxed);
			const auto base = reinterpret_cast<std::uintptr_t>(a_this);
			std::uintptr_t raw = 0;
			(void)Platform::SafeReadPointer(base + kPassResourceIndexOffset, raw);
			REX::INFO("[UiPassSeam] {} instance=0x{:X} resourceIndex(+0x{:X})=0x{:X}",
				a_tag, base, kPassResourceIndexOffset, static_cast<std::uint32_t>(raw) & 0xFFFFFF);
			const auto d3d12 = LocateD3D12Module();
			if (d3d12.base && Platform::IsReadableRange(base, 0x200)) {
				ScanBudget budget;
				ScanObject(a_tag, base, 0x200, 1, d3d12, budget);
				REX::INFO("[UiPassSeam] {} instance scan done ({} log slots left)", a_tag, budget.logs);
			}
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

		// -------------------------------------------- phase-2 seam draw (dev)
		// Records a solid premultiplied triangle into ScaleformCompositeBuffer
		// (config `uiPassDraw`) inside the hand-off barrier — see
		// tl_expectBufferTransition for why the draw lives there and nowhere
		// else. cmdCtx+0x60 (runtime-proven recording-list offset) is kept as
		// documentation for phase 3; the hand-off path gets the list for free.
		constexpr std::uintptr_t kCmdCtxListOffset = 0x60;

		std::atomic<bool> g_drawEnabled{ false };
		std::atomic<int> g_drawState{ 0 };  // 0 untried, 1 ready, -1 failed
		std::atomic<std::uint64_t> g_seamDraws{ 0 };
		std::atomic<std::uintptr_t> g_lastDrawBuffer{ 0 };   // churn diagnostics only
		std::atomic<std::uint32_t> g_bufferChangeLogs{ 0 };  // bounded (loads churn hard)

		ID3D12Device* g_drawDevice = nullptr;
		ID3D12RootSignature* g_drawRootSig = nullptr;
		ID3D12PipelineState* g_drawPso = nullptr;
		// RTV ring: transient buffers churn (loads) and two workers can record
		// different frames concurrently, so every draw writes a fresh slot
		// instead of racing a single cached view.
		constexpr std::uint32_t kRtvRingSlots = 8;
		ID3D12DescriptorHeap* g_drawRtvHeap = nullptr;
		std::uint32_t g_rtvStride = 0;
		std::atomic<std::uint32_t> g_rtvNext{ 0 };

		// Small corner triangle in clip space (bottom-left), so the debug draw
		// proves the seam without obscuring the screen.
		constexpr const char* kSeamVS = R"(
float4 main(uint id : SV_VertexID) : SV_Position {
    float2 p[3] = { float2(-0.95, -0.95), float2(-0.95, -0.35), float2(-0.35, -0.95) };
    return float4(p[id], 0.0, 1.0);
}
)";
		// Premultiplied 50% teal: the buffer's alpha drives both the engine's
		// composite blend and Frame Generation's UI composition.
		constexpr const char* kSeamPS = R"(
float4 main() : SV_Target { return float4(0.0, 0.35, 0.4, 0.5); }
)";

		[[nodiscard]] ID3DBlob* CompileSeamShader(const char* a_src, const char* a_target)
		{
			ID3DBlob* code = nullptr;
			ID3DBlob* errors = nullptr;
			const auto hr = ::D3DCompile(a_src, std::strlen(a_src), nullptr, nullptr, nullptr,
				"main", a_target, D3DCOMPILE_OPTIMIZATION_LEVEL3, 0, &code, &errors);
			if (FAILED(hr)) {
				REX::ERROR("[UiPassSeam] draw: shader '{}' compile failed (hr=0x{:08X}): {}",
					a_target, static_cast<std::uint32_t>(hr),
					errors ? static_cast<const char*>(errors->GetBufferPointer()) : "no message");
			}
			if (errors) {
				errors->Release();
			}
			return code;
		}

		// Lazy, once, on the first End seam with draw enabled.
		void EnsureDrawObjects()
		{
			int expected = 0;
			if (!g_drawState.compare_exchange_strong(expected, -1, std::memory_order_acq_rel)) {
				return;  // already tried; never retry
			}

			const auto engine = LocateEngineD3D12();
			if (!engine) {
				REX::WARN("[UiPassSeam] draw: engine device not reachable; seam draw disabled");
				return;
			}
			engine.directQueue->Release();  // draw records on the engine's list; no queue use
			g_drawDevice = reinterpret_cast<ID3D12Device*>(engine.device);

			auto* vs = CompileSeamShader(kSeamVS, "vs_5_0");
			auto* ps = CompileSeamShader(kSeamPS, "ps_5_0");

			D3D12_ROOT_SIGNATURE_DESC rsDesc{};  // no parameters: constants live in the shader
			ID3DBlob* rsBlob = nullptr;
			ID3DBlob* rsError = nullptr;
			bool ok = vs && ps &&
			          SUCCEEDED(::D3D12SerializeRootSignature(&rsDesc, D3D_ROOT_SIGNATURE_VERSION_1, &rsBlob, &rsError)) &&
			          SUCCEEDED(g_drawDevice->CreateRootSignature(0, rsBlob->GetBufferPointer(), rsBlob->GetBufferSize(),
						  __uuidof(ID3D12RootSignature), reinterpret_cast<void**>(&g_drawRootSig)));
			if (rsBlob) {
				rsBlob->Release();
			}
			if (rsError) {
				rsError->Release();
			}

			if (ok) {
				D3D12_GRAPHICS_PIPELINE_STATE_DESC desc{};
				desc.pRootSignature = g_drawRootSig;
				desc.VS = { vs->GetBufferPointer(), vs->GetBufferSize() };
				desc.PS = { ps->GetBufferPointer(), ps->GetBufferSize() };
				desc.SampleMask = UINT_MAX;
				desc.RasterizerState.FillMode = D3D12_FILL_MODE_SOLID;
				desc.RasterizerState.CullMode = D3D12_CULL_MODE_NONE;
				desc.RasterizerState.DepthClipEnable = TRUE;
				desc.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
				desc.NumRenderTargets = 1;
				// The buffer is R8G8B8A8_TYPELESS; the engine's own RTV choice is
				// unknown, so start with the plain UNORM view (capture 2026-07-21).
				desc.RTVFormats[0] = DXGI_FORMAT_R8G8B8A8_UNORM;
				desc.SampleDesc.Count = 1;
				auto& rt = desc.BlendState.RenderTarget[0];
				rt.BlendEnable = TRUE;
				rt.SrcBlend = D3D12_BLEND_ONE;  // premultiplied over
				rt.DestBlend = D3D12_BLEND_INV_SRC_ALPHA;
				rt.BlendOp = D3D12_BLEND_OP_ADD;
				rt.SrcBlendAlpha = D3D12_BLEND_ONE;
				rt.DestBlendAlpha = D3D12_BLEND_INV_SRC_ALPHA;
				rt.BlendOpAlpha = D3D12_BLEND_OP_ADD;
				rt.RenderTargetWriteMask = D3D12_COLOR_WRITE_ENABLE_ALL;
				ok = SUCCEEDED(g_drawDevice->CreateGraphicsPipelineState(&desc,
					__uuidof(ID3D12PipelineState), reinterpret_cast<void**>(&g_drawPso)));
			}

			if (ok) {
				D3D12_DESCRIPTOR_HEAP_DESC heap{};
				heap.Type = D3D12_DESCRIPTOR_HEAP_TYPE_RTV;
				heap.NumDescriptors = kRtvRingSlots;
				ok = SUCCEEDED(g_drawDevice->CreateDescriptorHeap(&heap,
					__uuidof(ID3D12DescriptorHeap), reinterpret_cast<void**>(&g_drawRtvHeap)));
				g_rtvStride = g_drawDevice->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_RTV);
			}

			if (vs) {
				vs->Release();
			}
			if (ps) {
				ps->Release();
			}
			if (ok) {
				g_drawState.store(1, std::memory_order_release);
				REX::INFO("[UiPassSeam] draw: pipeline ready (RTV view R8G8B8A8_UNORM on the typeless buffer)");
			} else {
				REX::WARN("[UiPassSeam] draw: pipeline setup failed; seam draw disabled");
			}
		}

		// Inside the hand-off barrier: a_buffer is THIS frame's UI buffer,
		// still in RENDER_TARGET state (the transition is forwarded after we
		// return), and a_list is the recording list it was drawn on.
		void RecordSeamDrawAtHandoff(ID3D12GraphicsCommandList* a_list, ID3D12Resource* a_buffer)
		{
			if (!g_drawEnabled.load(std::memory_order_relaxed) || !a_list || !a_buffer) {
				return;
			}

			if constexpr (kSeamDebugTriangle) {
				EnsureDrawObjects();
				if (g_drawState.load(std::memory_order_acquire) != 1) {
					return;
				}

				const auto desc = a_buffer->GetDesc();
				const auto slot = g_rtvNext.fetch_add(1, std::memory_order_relaxed) % kRtvRingSlots;
				D3D12_CPU_DESCRIPTOR_HANDLE rtv{ g_drawRtvHeap->GetCPUDescriptorHandleForHeapStart().ptr +
					static_cast<SIZE_T>(slot) * g_rtvStride };
				D3D12_RENDER_TARGET_VIEW_DESC rtvDesc{};
				rtvDesc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;  // typed view on the typeless buffer
				rtvDesc.ViewDimension = D3D12_RTV_DIMENSION_TEXTURE2D;
				g_drawDevice->CreateRenderTargetView(a_buffer, &rtvDesc, rtv);

				const D3D12_VIEWPORT vp{ 0.0f, 0.0f, static_cast<float>(desc.Width), static_cast<float>(desc.Height), 0.0f, 1.0f };
				const D3D12_RECT scissor{ 0, 0, static_cast<LONG>(desc.Width), static_cast<LONG>(desc.Height) };

				a_list->OMSetRenderTargets(1, &rtv, FALSE, nullptr);
				a_list->RSSetViewports(1, &vp);
				a_list->RSSetScissorRects(1, &scissor);
				a_list->SetGraphicsRootSignature(g_drawRootSig);
				a_list->SetPipelineState(g_drawPso);
				a_list->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
				a_list->DrawInstanced(3, 1, 0, 0);
			} else {
				// Real overlay (phase 3): the compositor records its quad — the
				// shared-ring texture, premultiplied over — onto the engine's
				// list. Snapshot the engine's descriptor-heap binding FIRST:
				// the compositor binds its own shader-visible heap (the quad
				// samples an SRV), and the snapshot is taken before that bind
				// runs through our SetDescriptorHeaps thunk and overwrites the
				// thread-local tracking.
				ID3D12DescriptorHeap* engineHeaps[2]{ tl_heaps[0], tl_heaps[1] };
				const UINT engineHeapCount = tl_heapCount;
				const bool heapKnown = engineHeapCount > 0 && tl_heapList == a_list;

				if (!RecordSeamOverlayDraw(a_list, a_buffer)) {
					return;  // hidden, no ready GPU frame, or non-GPU transport
				}
				if (heapKnown) {
					// Restore the engine's heaps: cached-state layers downstream
					// may legally skip a "redundant" rebind.
					if (const auto original = g_origSetDescriptorHeaps.load(std::memory_order_relaxed)) {
						original(a_list, engineHeapCount, engineHeaps);
					}
				}
			}

			const auto draws = g_seamDraws.fetch_add(1, std::memory_order_relaxed) + 1;
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

		// ------------------------------------------------------------- thunks
		void* BeginThunk(void* a_this, void* a_ctx, void* a_io, void* a_r9)
		{
			const auto count = g_begin.count.fetch_add(1, std::memory_order_relaxed) + 1;

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
			const bool allowWindow = g_captureWindows.load(std::memory_order_relaxed) < kMaxCaptureWindows;
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
			if (count <= kMaxDetailLogs) {
				ScanPassInstance("BeginPass", g_begin, a_this);
			}

			const auto original = reinterpret_cast<ExecuteFn>(g_origBegin.load(std::memory_order_relaxed));
			void* result = original ? original(a_this, a_ctx, a_io, a_r9) : nullptr;

			// Post-original: ScaleformBegin has just bound the HAL fields.
			if (count <= kMaxDetailLogs) {
				const auto halRT = ReadHalField(kHalRTOffset);
				const auto halCtx = ReadHalField(kHalCtxOffset);
				const auto getterCtx = reinterpret_cast<std::uintptr_t>(GetEngineCmdCtx(a_ctx));
				// getterCtx == halCtx is the runtime proof that the assumed
				// getter ABI (this-only, result in rax) is right.
				REX::INFO("[UiPassSeam] Begin[{}] tid={} ctx=0x{:X} io=0x{:X} hal+0x1410=0x{:X} hal+0x1418=0x{:X} getter(ctx)=0x{:X}{}",
					count, ::GetCurrentThreadId(),
					reinterpret_cast<std::uintptr_t>(a_ctx), reinterpret_cast<std::uintptr_t>(a_io),
					halRT, halCtx, getterCtx,
					(getterCtx && getterCtx == halCtx) ? " [getter ABI CONFIRMED]" : "");

				if (g_begin.scans.load(std::memory_order_relaxed) < kMaxScans && (halRT || halCtx)) {
					g_begin.scans.fetch_add(1, std::memory_order_relaxed);
					const auto d3d12 = LocateD3D12Module();
					if (d3d12.base) {
						ScanBudget budget;
						if (halRT && Platform::IsReadableRange(halRT, 0x100)) {
							ScanObject("halRT", halRT, 0x100, 1, d3d12, budget);
						}
						if (halCtx && Platform::IsReadableRange(halCtx, 0x200)) {
							ScanObject("halCtx", halCtx, 0x200, 1, d3d12, budget);
						}
						REX::INFO("[UiPassSeam] Begin[{}] scan done ({} log slots left)", count, budget.logs);
					}
				}
			}
			if (window) {
				REX::INFO("[UiPassSeam] window: Begin exit");
			}
			return result;
		}

		void* EndThunk(void* a_this, void* a_ctx, void* a_io, void* a_r9)
		{
			const auto count = g_end.count.fetch_add(1, std::memory_order_relaxed) + 1;
			const bool window = g_captureTid.load(std::memory_order_relaxed) == ::GetCurrentThreadId();
			if (window) {
				REX::INFO("[UiPassSeam] window: End enter");
			}
			if (count <= kMaxDetailLogs) {
				REX::INFO("[UiPassSeam] End[{}] tid={} this=0x{:X} ctx=0x{:X} io=0x{:X} cmdCtx=0x{:X}",
					count, ::GetCurrentThreadId(),
					reinterpret_cast<std::uintptr_t>(a_this), reinterpret_cast<std::uintptr_t>(a_ctx),
					reinterpret_cast<std::uintptr_t>(a_io),
					reinterpret_cast<std::uintptr_t>(GetEngineCmdCtx(a_ctx)));
				ScanPassInstance("EndPass", g_end, a_this);
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
			const auto count = g_composite.count.fetch_add(1, std::memory_order_relaxed) + 1;
			const bool window = g_captureTid.load(std::memory_order_relaxed) == ::GetCurrentThreadId();
			if (window) {
				REX::INFO("[UiPassSeam] window: Composite enter");
			}

			// Pre-original probe of the state a draw would see here.
			std::uintptr_t cmdCtx = 0;
			if (count <= kMaxDetailLogs) {
				cmdCtx = reinterpret_cast<std::uintptr_t>(GetEngineCmdCtx(a_ctx));
				REX::INFO("[UiPassSeam] Composite[{}] tid={} this=0x{:X} ctx=0x{:X} io=0x{:X} cmdCtx=0x{:X}",
					count, ::GetCurrentThreadId(),
					reinterpret_cast<std::uintptr_t>(a_this), reinterpret_cast<std::uintptr_t>(a_ctx),
					reinterpret_cast<std::uintptr_t>(a_io), cmdCtx);

				if (g_composite.scans.load(std::memory_order_relaxed) < kMaxScans && cmdCtx) {
					g_composite.scans.fetch_add(1, std::memory_order_relaxed);
					const auto d3d12 = LocateD3D12Module();
					if (d3d12.base && Platform::IsReadableRange(cmdCtx, 0x200)) {
						ScanBudget budget;
						ScanObject("cmdCtx", cmdCtx, 0x200, 1, d3d12, budget);
						REX::INFO("[UiPassSeam] Composite[{}] scan done ({} log slots left)", count, budget.logs);
					}
				}
				ScanPassInstance("CompositePass", g_composite, a_this);
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
			ExecuteFn a_thunk)
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
			*slot = reinterpret_cast<void*>(a_thunk);
			::VirtualProtect(slot, sizeof(void*), oldProtect, &oldProtect);
			REX::INFO("[UiPassSeam] hooked {} slot 7 (vtbl 0x{:X}, original 0x{:X})", a_label, vtbl.address(), current);
			return current;
		}
	}

	bool Install(const bool a_draw)
	{
		if (g_installed.exchange(true, std::memory_order_relaxed)) {
			return true;
		}
		g_drawEnabled.store(a_draw, std::memory_order_relaxed);
		if (a_draw) {
			REX::INFO("[UiPassSeam] seam DRAW enabled (uiPassDraw): debug triangle into "
					  "ScaleformCompositeBuffer at the ScaleformEnd seam");
		}

		const auto origBegin = HookExecuteSlot(
			"ScaleformBegin", kVtblScaleformBegin, kIdBeginExecute, &BeginThunk);
		const auto origEnd = HookExecuteSlot(
			"ScaleformEnd", kVtblScaleformEnd, kIdEndExecute, &EndThunk);
		const auto origComposite = HookExecuteSlot(
			"ScaleformComposite", kVtblScaleformComposite, kIdCompositeExecute, &CompositeThunk);
		g_origBegin.store(origBegin, std::memory_order_relaxed);
		g_origEnd.store(origEnd, std::memory_order_relaxed);
		g_origComposite.store(origComposite, std::memory_order_relaxed);

		const bool ok = origBegin != 0 && origEnd != 0 && origComposite != 0;
		if (!ok) {
			REX::WARN("[UiPassSeam] probe incomplete — see slot warnings above (unhooked passes run untouched)");
		}
		return ok;
	}
}
