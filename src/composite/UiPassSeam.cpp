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

#include <atomic>
#include <cstddef>
#include <cstdint>

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

		// RenderPass vtable slot 7 = ExecuteRenderPass(this, GraphContext*, IOHandles*).
		constexpr std::size_t kExecuteSlot = 7;

		using ExecuteFn = void* (*)(void*, void*, void*, void*);

		std::atomic<std::uintptr_t> g_origBegin{ 0 };
		std::atomic<std::uintptr_t> g_origEnd{ 0 };
		std::atomic<std::uintptr_t> g_origComposite{ 0 };
		std::atomic<bool> g_installed{ false };
		std::atomic<bool> g_installOk{ false };

		// -------------------------------------------------- D3D12 seam hooks
		// The seam draws from inside the engine's own command recording, so it
		// hooks the process-wide ID3D12GraphicsCommandList vtable (obtained from
		// a throwaway list created on the game's own device — same trick as the
		// Present hook's throwaway swapchain) at four slots (ResourceBarrier is
		// the hand-off match/draw point; SetDescriptorHeaps, SetGraphicsRootSignature
		// and SetPipelineState track the three states the seam draw clobbers so it
		// can restore each — see RecordSeamDrawAtHandoff):
		//
		//   ResourceBarrier    — the hand-off match/draw point. The engine
		//                        transitions each UI buffer out of RENDER_TARGET
		//                        right after ScaleformEnd; the thunk draws the
		//                        overlay into the buffer just before forwarding
		//                        that barrier.
		//   SetDescriptorHeaps — tracks the engine's bound heaps so the seam
		//                        draw can restore them after binding its own.
		//
		// Slot indices are fixed COM ABI, straight from the d3d12.h C vtable
		// (ID3D12GraphicsCommandListVtbl): 26 = ResourceBarrier,
		// 28 = SetDescriptorHeaps. Trust but verify: after hooking, each slot is
		// self-tested by calling it on our own throwaway list and checking the
		// thunk observed the sentinel — a wrong index unhooks itself.
		constexpr std::size_t kSlotResourceBarrier = 26;
		constexpr std::size_t kSlotSetDescriptorHeaps = 28;
		// The seam draw also clobbers the engine's bound pipeline state and
		// graphics root signature. Both are tracked (below) so the draw can
		// restore them, for the same reason the heaps are — see the comment on
		// tl_heaps. Slot indices from the d3d12.h C vtable, self-tested below:
		// 25 = SetPipelineState, 30 = SetGraphicsRootSignature.
		constexpr std::size_t kSlotSetPipelineState = 25;
		constexpr std::size_t kSlotSetGraphicsRootSignature = 30;

		using ResourceBarrierFn = void(STDMETHODCALLTYPE*)(
			ID3D12GraphicsCommandList*, UINT, const D3D12_RESOURCE_BARRIER*);
		using SetDescriptorHeapsFn = void(STDMETHODCALLTYPE*)(
			ID3D12GraphicsCommandList*, UINT, ID3D12DescriptorHeap* const*);
		using SetGraphicsRootSignatureFn = void(STDMETHODCALLTYPE*)(
			ID3D12GraphicsCommandList*, ID3D12RootSignature*);
		using SetPipelineStateFn = void(STDMETHODCALLTYPE*)(
			ID3D12GraphicsCommandList*, ID3D12PipelineState*);

		std::atomic<ResourceBarrierFn> g_origResourceBarrier{ nullptr };
		std::atomic<SetDescriptorHeapsFn> g_origSetDescriptorHeaps{ nullptr };
		std::atomic<SetGraphicsRootSignatureFn> g_origSetGraphicsRootSignature{ nullptr };
		std::atomic<SetPipelineStateFn> g_origSetPipelineState{ nullptr };

		// The engine's live descriptor-heap binding, tracked per thread: the
		// real-overlay seam draw binds the compositor's shader-visible heap on
		// the engine's list and must restore the engine's afterwards — the
		// engine's abstraction layer caches bound state and may legally skip a
		// "redundant" rebind downstream.
		thread_local ID3D12GraphicsCommandList* tl_heapList = nullptr;
		thread_local ID3D12DescriptorHeap* tl_heaps[2] = {};
		thread_local UINT tl_heapCount = 0;
		// Same rationale as tl_heaps, for the other two states the seam draw
		// clobbers and the engine's abstraction shadow-caches: the graphics root
		// signature and the pipeline state. If either isn't observable for the
		// hand-off list (the recording worker didn't set it this frame), the
		// seam skips the draw rather than leave it clobbered and unrestored.
		thread_local ID3D12GraphicsCommandList* tl_rootSigList = nullptr;
		thread_local ID3D12RootSignature* tl_rootSig = nullptr;
		thread_local ID3D12GraphicsCommandList* tl_psoList = nullptr;
		thread_local ID3D12PipelineState* tl_pso = nullptr;
		std::atomic<int> g_hookInstallState{ 0 };  // 0 untried, 1 ready, -1 failed

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
		std::atomic<bool> g_selfTestBarrierSeen{ false };
		std::atomic<bool> g_selfTestHeapsSeen{ false };
		std::atomic<bool> g_selfTestRootSigSeen{ false };
		std::atomic<bool> g_selfTestPsoSeen{ false };
		// One-time log when a hand-off draw is skipped because the engine's
		// clobbered state can't be restored for the recording list.
		std::atomic<bool> g_restoreSkipLogged{ false };

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

		void STDMETHODCALLTYPE SetGraphicsRootSignatureThunk(
			ID3D12GraphicsCommandList* a_self,
			ID3D12RootSignature* a_rootSig)
		{
			if (a_self == g_selfTestList) {
				g_selfTestRootSigSeen.store(true, std::memory_order_relaxed);
			} else {
				tl_rootSigList = a_self;
				tl_rootSig = a_rootSig;
			}
			if (const auto original = g_origSetGraphicsRootSignature.load(std::memory_order_relaxed)) {
				original(a_self, a_rootSig);
			}
		}

		void STDMETHODCALLTYPE SetPipelineStateThunk(
			ID3D12GraphicsCommandList* a_self,
			ID3D12PipelineState* a_pso)
		{
			if (a_self == g_selfTestList) {
				g_selfTestPsoSeen.store(true, std::memory_order_relaxed);
			} else {
				tl_psoList = a_self;
				tl_pso = a_pso;
			}
			if (const auto original = g_origSetPipelineState.load(std::memory_order_relaxed)) {
				original(a_self, a_pso);
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
		// reach the shared d3d12.dll vtable, hooks the two slots the seam draw
		// needs, and proves the slot indices by calling both methods on the
		// throwaway list (a global-UAV barrier and clearing heaps are both legal
		// no-ops). A failed self-test restores the slots.
		void EnsureDrawHooksInstalled()
		{
			int expected = 0;
			if (!g_hookInstallState.compare_exchange_strong(expected, -1, std::memory_order_acq_rel)) {
				return;  // already tried (state is 1 or -1); never retry
			}

			const auto engine = LocateEngineD3D12();
			if (!engine) {
				REX::WARN("[UiPassSeam] draw hooks: engine D3D12 device not reachable; seam draw disabled");
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
				// Publish each forward target into its atomic BEFORE its thunk becomes
				// reachable on the shared d3d12.dll vtable (release store), so a
				// concurrent render worker routing through a just-installed thunk can
				// never load a null original and drop the engine's D3D12 call. All
				// four are load-bearing for the seam draw: ResourceBarrier is the
				// hand-off match/draw point; SetDescriptorHeaps, SetGraphicsRootSignature
				// and SetPipelineState track the engine's bound state so the draw can
				// restore each of the three states it clobbers.
				const auto origBarrier = reinterpret_cast<ResourceBarrierFn>(vtbl[kSlotResourceBarrier]);
				const auto origHeaps = reinterpret_cast<SetDescriptorHeapsFn>(vtbl[kSlotSetDescriptorHeaps]);
				const auto origRootSig = reinterpret_cast<SetGraphicsRootSignatureFn>(vtbl[kSlotSetGraphicsRootSignature]);
				const auto origPso = reinterpret_cast<SetPipelineStateFn>(vtbl[kSlotSetPipelineState]);
				g_origResourceBarrier.store(origBarrier, std::memory_order_release);
				g_origSetDescriptorHeaps.store(origHeaps, std::memory_order_release);
				g_origSetGraphicsRootSignature.store(origRootSig, std::memory_order_release);
				g_origSetPipelineState.store(origPso, std::memory_order_release);
				const bool patchedBarrier =
					PatchSlot(vtbl, kSlotResourceBarrier, reinterpret_cast<void*>(&ResourceBarrierThunk)) != nullptr;
				const bool patchedHeaps =
					PatchSlot(vtbl, kSlotSetDescriptorHeaps, reinterpret_cast<void*>(&SetDescriptorHeapsThunk)) != nullptr;
				const bool patchedRootSig =
					PatchSlot(vtbl, kSlotSetGraphicsRootSignature, reinterpret_cast<void*>(&SetGraphicsRootSignatureThunk)) != nullptr;
				const bool patchedPso =
					PatchSlot(vtbl, kSlotSetPipelineState, reinterpret_cast<void*>(&SetPipelineStateThunk)) != nullptr;

				// Prove the slot indices by calling each hooked method on the throwaway
				// list (all legal no-ops) and checking the thunk saw the sentinel — a
				// wrong index unhooks itself.
				const bool patched = patchedBarrier && patchedHeaps && patchedRootSig && patchedPso;
				if (patched) {
					D3D12_RESOURCE_BARRIER uav{};
					uav.Type = D3D12_RESOURCE_BARRIER_TYPE_UAV;
					uav.UAV.pResource = nullptr;  // global UAV barrier: legal on any list
					list->ResourceBarrier(1, &uav);
					list->SetDescriptorHeaps(0, nullptr);       // clearing heaps: legal no-op
					list->SetGraphicsRootSignature(nullptr);    // unbinding root sig: legal no-op
					list->SetPipelineState(nullptr);            // unbinding PSO: legal no-op
				}

				const bool barrierOk = g_selfTestBarrierSeen.load(std::memory_order_relaxed);
				const bool heapsOk = g_selfTestHeapsSeen.load(std::memory_order_relaxed);
				const bool rootSigOk = g_selfTestRootSigSeen.load(std::memory_order_relaxed);
				const bool psoOk = g_selfTestPsoSeen.load(std::memory_order_relaxed);
				if (patched && barrierOk && heapsOk && rootSigOk && psoOk) {
					g_selfTestList = nullptr;
					g_hookInstallState.store(1, std::memory_order_release);
					REX::DEBUG("[UiPassSeam] seam draw hooks armed: ID3D12GraphicsCommandList vtable slots {} (barrier) / {} "
							  "(SetDescriptorHeaps) / {} (SetGraphicsRootSignature) / {} (SetPipelineState) hooked and self-tested",
						kSlotResourceBarrier, kSlotSetDescriptorHeaps, kSlotSetGraphicsRootSignature, kSlotSetPipelineState);
				} else {
					// Wrong slot layout or protect failure: undo what we did.
					if (patchedBarrier) {
						(void)PatchSlot(vtbl, kSlotResourceBarrier, reinterpret_cast<void*>(origBarrier));
					}
					if (patchedHeaps) {
						(void)PatchSlot(vtbl, kSlotSetDescriptorHeaps, reinterpret_cast<void*>(origHeaps));
					}
					if (patchedRootSig) {
						(void)PatchSlot(vtbl, kSlotSetGraphicsRootSignature, reinterpret_cast<void*>(origRootSig));
					}
					if (patchedPso) {
						(void)PatchSlot(vtbl, kSlotSetPipelineState, reinterpret_cast<void*>(origPso));
					}
					g_origResourceBarrier.store(nullptr, std::memory_order_relaxed);
					g_origSetDescriptorHeaps.store(nullptr, std::memory_order_relaxed);
					g_origSetGraphicsRootSignature.store(nullptr, std::memory_order_relaxed);
					g_origSetPipelineState.store(nullptr, std::memory_order_relaxed);
					g_selfTestList = nullptr;
					REX::WARN("[UiPassSeam] seam draw hook self-test FAILED (patch b/h/r/p={}/{}/{}/{} seen b/h/r/p={}/{}/{}/{}); vtable restored, seam draw disabled",
						patchedBarrier, patchedHeaps, patchedRootSig, patchedPso, barrierOk, heapsOk, rootSigOk, psoOk);
				}
				list->Close();
			} else {
				REX::WARN("[UiPassSeam] draw hooks: throwaway command list creation failed; seam draw disabled");
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

		// ------------------------------------------------------ seam draw state
		std::atomic<bool> g_drawEnabled{ false };

		// Inside the hand-off barrier: a_buffer is THIS frame's UI buffer,
		// still in RENDER_TARGET state (the transition is forwarded after we
		// return), and a_list is the recording list it was drawn on.
		void RecordSeamDrawAtHandoff(ID3D12GraphicsCommandList* a_list, ID3D12Resource* a_buffer,
			const bool a_fgTarget, const bool a_regionFirst)
		{
			if (!g_drawEnabled.load(std::memory_order_relaxed) || !a_list || !a_buffer) {
				return;
			}

			// Snapshot the engine's bound state (descriptor heaps, graphics root
			// signature, pipeline state) before the compositor clobbers all three
			// with its own. The engine's abstraction shadow-caches these and skips
			// "redundant" rebinds downstream, so a clobber left unrestored makes
			// the engine draw the Scaleform composite against our state — a black
			// HUD and menu until that state happens to change again.
			//
			// Every piece must be observable for THIS list (the recording worker
			// set it this frame). If any isn't — pass execution moved among render
			// workers, or the engine's own shadow skipped the set — we cannot
			// guarantee a full restore, so skip the draw entirely rather than
			// corrupt the engine. The overlay is invisible for that frame, never
			// the game. In the normal path ScaleformEnd just drew, so the engine
			// set all three on this list and the snapshots are valid.
			ID3D12DescriptorHeap* engineHeaps[2]{ tl_heaps[0], tl_heaps[1] };
			const UINT engineHeapCount = tl_heapCount;
			ID3D12RootSignature* const engineRootSig = tl_rootSig;
			ID3D12PipelineState* const enginePso = tl_pso;
			const bool restorable =
				engineHeapCount > 0 && tl_heapList == a_list &&
				tl_rootSigList == a_list &&
				tl_psoList == a_list;
			if (!restorable) {
				if (!g_restoreSkipLogged.exchange(true, std::memory_order_relaxed)) {
					REX::DEBUG("[UiPassSeam] hand-off draw skipped: engine state not fully observable for the "
							  "recording list (heaps/rootsig/pso lists match={}/{}/{}, heapCount={}) — "
							  "overlay withheld this frame to avoid clobbering engine state",
						tl_heapList == a_list, tl_rootSigList == a_list, tl_psoList == a_list, engineHeapCount);
				}
				return;
			}

			if (!RecordSeamOverlayDraw(a_list, a_buffer, a_fgTarget, a_regionFirst)) {
				return;
			}

			// Restore, so the engine's list state matches its shadow again.
			if (const auto original = g_origSetDescriptorHeaps.load(std::memory_order_relaxed)) {
				original(a_list, engineHeapCount, engineHeaps);
			}
			if (const auto original = g_origSetGraphicsRootSignature.load(std::memory_order_relaxed)) {
				original(a_list, engineRootSig);
			}
			if (const auto original = g_origSetPipelineState.load(std::memory_order_relaxed)) {
				original(a_list, enginePso);
			}
		}

		// ------------------------------------------------------------- thunks
		void* BeginThunk(void* a_this, void* a_ctx, void* a_io, void* a_r9)
		{
			// Install the barrier/heaps hooks the seam draw needs, once, on the
			// first pass call (the engine's device is live by now).
			EnsureDrawHooksInstalled();

			const auto original = reinterpret_cast<ExecuteFn>(g_origBegin.load(std::memory_order_relaxed));
			return original ? original(a_this, a_ctx, a_io, a_r9) : nullptr;
		}

		void* EndThunk(void* a_this, void* a_ctx, void* a_io, void* a_r9)
		{
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
			return result;
		}

		void* CompositeThunk(void* a_this, void* a_ctx, void* a_io, void* a_r9)
		{
			tl_handoffDrawsLeft = 0;  // hand-off glue region over for this frame
			tl_callsAfterFirstDraw = -1;

			const auto original = reinterpret_cast<ExecuteFn>(g_origComposite.load(std::memory_order_relaxed));
			return original ? original(a_this, a_ctx, a_io, a_r9) : nullptr;
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
			REX::DEBUG("[UiPassSeam] hooked {} slot 7 (vtbl 0x{:X}, original 0x{:X})", a_label, vtbl.address(), current);
			return current;
		}
	}

	bool Install(const bool a_draw)
	{
		if (g_installed.exchange(true, std::memory_order_relaxed)) {
			return g_installOk.load(std::memory_order_acquire);
		}

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
			REX::DEBUG("[UiPassSeam] seam draw enabled: overlay records into Starfield's "
					  "transparent UI layer at the ScaleformEnd hand-off");
		}
		return ok;
	}
}
