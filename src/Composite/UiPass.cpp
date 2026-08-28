#include "Composite/UiPass.h"

#include "Composite/D3D12Compositor.h"  // RecordOverlayIntoRenderTarget
#include "Composite/EngineD3D12.h"
#include "Composite/UiPassPolicy.h"
#include "Composite/UiTargetFormat.h"
#include "Core/Log.h"
#include "Platform/WindowsPlatform.h"

#include "Composite/D3D12Prologue.h"  // GDI-free <Windows.h> + <d3d12.h>

#include "RE/IDs_VTABLE.h"
#include "REL/Utility.h"

#include <atomic>
#include <cstddef>
#include <cstdint>

namespace OSFUI::UiPass
{
	namespace
	{
		constexpr std::size_t kExecuteSlot = 7;

		using ExecuteFn = void* (*)(void*, void*, void*, void*);

		std::atomic<std::uintptr_t> g_origBegin{ 0 };
		std::atomic<std::uintptr_t> g_origEnd{ 0 };
		std::atomic<std::uintptr_t> g_origComposite{ 0 };
		std::atomic<bool> g_installed{ false };
		std::atomic<bool> g_installOk{ false };

		// Hook only the handoff barrier and descriptor heaps.
		constexpr std::size_t kSlotResourceBarrier = 26;
		constexpr std::size_t kSlotSetDescriptorHeaps = 28;

		using ResourceBarrierFn = void(STDMETHODCALLTYPE*)(ID3D12GraphicsCommandList*, UINT, const D3D12_RESOURCE_BARRIER*);
		using SetDescriptorHeapsFn = void(STDMETHODCALLTYPE*)(ID3D12GraphicsCommandList*, UINT, ID3D12DescriptorHeap* const*);

		std::atomic<ResourceBarrierFn> g_origResourceBarrier{ nullptr };
		std::atomic<SetDescriptorHeapsFn> g_origSetDescriptorHeaps{ nullptr };

		// Continuously track the engine's last heap binding so a pass that inherits descriptor state can still be restored after overlay recording.
		thread_local ID3D12GraphicsCommandList* tl_heapList = nullptr;
		thread_local ID3D12DescriptorHeap* tl_heaps[2] = {};
		thread_local UINT tl_heapCount = 0;
		thread_local detail::ScaleformHandoffWindow tl_handoffWindow;

		// Suppress tracking of the compositor's heap or the next handoff restores the wrong engine state.
		thread_local bool tl_inOverlayDraw = false;
		std::atomic<detail::CommandListHookState> g_hookInstallState{
			detail::CommandListHookState::Uninitialized
		};
		// The lazy hook self-test can disable drawing after Install() succeeds.
		std::atomic<bool> g_drawEnabled{ false };
		detail::FrameGenerationTargetPolicy g_fgTargetPolicy;
		std::atomic_bool g_fgLayerOnlyLogged{ false };

		void RecordOverlayAtHandoff(ID3D12GraphicsCommandList* a_list, ID3D12Resource* a_buffer, bool a_fgTarget, bool a_regionFirst);
		std::atomic<ID3D12GraphicsCommandList*> g_selfTestList{ nullptr };
		std::atomic<bool> g_selfTestBarrierSeen{ false };
		std::atomic<bool> g_selfTestHeapsSeen{ false };
		std::atomic_bool g_unknownEngineHeapLogged{ false };

		void STDMETHODCALLTYPE SetDescriptorHeapsThunk(ID3D12GraphicsCommandList* a_self, const UINT a_num, ID3D12DescriptorHeap* const* a_heaps)
		{
			if (a_self == g_selfTestList.load(std::memory_order_acquire)) {
				g_selfTestHeapsSeen.store(true, std::memory_order_relaxed);
			} else if (!tl_inOverlayDraw) {
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

		void STDMETHODCALLTYPE ResourceBarrierThunk(
			ID3D12GraphicsCommandList* a_self,
			const UINT a_numBarriers,
			const D3D12_RESOURCE_BARRIER* a_barriers)
		{
			tl_handoffWindow.OnBarrierCall();
			if (tl_handoffWindow.HandoffArmed() && a_barriers) {
				for (UINT i = 0; i < a_numBarriers && tl_handoffWindow.HandoffArmed(); ++i) {
					const auto& barrier = a_barriers[i];
					if (barrier.Type != D3D12_RESOURCE_BARRIER_TYPE_TRANSITION ||
						!barrier.Transition.pResource ||
						barrier.Transition.StateBefore != D3D12_RESOURCE_STATE_RENDER_TARGET ||
						!(barrier.Transition.StateAfter &
							(D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE | D3D12_RESOURCE_STATE_COPY_SOURCE))) {
						continue;
					}
					const auto desc = barrier.Transition.pResource->GetDesc();
					if (UiTargetFormat::ResolveRtv(desc.Format) == DXGI_FORMAT_UNKNOWN ||
						desc.Dimension != D3D12_RESOURCE_DIMENSION_TEXTURE2D ||
						desc.SampleDesc.Count != 1 ||
						desc.Width < 256 || desc.Height < 256) {
						continue;
					}
					if (a_self->GetType() != D3D12_COMMAND_LIST_TYPE_DIRECT) {
						continue;
					}
					const bool fgTarget = (barrier.Transition.StateAfter & D3D12_RESOURCE_STATE_COPY_SOURCE) != 0 && (barrier.Transition.StateAfter & D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE) == 0;
					const bool regionFirst = tl_handoffWindow.ConsumeAndReportFirstCandidate();
					RecordOverlayAtHandoff(a_self, barrier.Transition.pResource, fgTarget, regionFirst);
				}
			}

			if (a_self == g_selfTestList.load(std::memory_order_acquire)) {
				g_selfTestBarrierSeen.store(true, std::memory_order_relaxed);
			}
			if (const auto original = g_origResourceBarrier.load(std::memory_order_relaxed)) {
				original(a_self, a_numBarriers, a_barriers);
			}
		}

		[[nodiscard]] void* PatchSlot(void** a_vtbl, const std::size_t a_slot, void* a_thunk)
		{
			void* original = a_vtbl[a_slot];
			if (!REL::WriteSafeData(&a_vtbl[a_slot], a_thunk)) {
				return nullptr;
			}
			return original;
		}

		void MarkDrawHooksFailed()
		{
			g_drawEnabled.store(false, std::memory_order_release);
			g_hookInstallState.store(
				detail::CommandListHookState::Failed, std::memory_order_release);
		}

		void EnsureDrawHooksInstalled()
		{
			auto expected = detail::CommandListHookState::Uninitialized;
			if (!g_hookInstallState.compare_exchange_strong(
					expected, detail::CommandListHookState::Installing,
					std::memory_order_acq_rel)) {
				return;
			}

			const auto engine = LocateEngineD3D12();
			if (!engine) {
				MarkDrawHooksFailed();
				REX::ERROR("[UiPass] draw hooks: engine D3D12 device not reachable; UI-pass draw disabled");
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
				g_selfTestList.store(list, std::memory_order_release);
				const auto origBarrier = reinterpret_cast<ResourceBarrierFn>(vtbl[kSlotResourceBarrier]);
				const auto origHeaps = reinterpret_cast<SetDescriptorHeapsFn>(vtbl[kSlotSetDescriptorHeaps]);
				g_origResourceBarrier.store(origBarrier, std::memory_order_release);
				g_origSetDescriptorHeaps.store(origHeaps, std::memory_order_release);
				const bool patchedBarrier =
					PatchSlot(vtbl, kSlotResourceBarrier,
						reinterpret_cast<void*>(&ResourceBarrierThunk)) != nullptr;
				const bool patchedHeaps =
					PatchSlot(vtbl, kSlotSetDescriptorHeaps,
						reinterpret_cast<void*>(&SetDescriptorHeapsThunk)) != nullptr;

				const bool patched = patchedBarrier && patchedHeaps;
				if (patched) {
					D3D12_RESOURCE_BARRIER uav{};
					uav.Type = D3D12_RESOURCE_BARRIER_TYPE_UAV;
					uav.UAV.pResource = nullptr;
					list->ResourceBarrier(1, &uav);
					list->SetDescriptorHeaps(0, nullptr);
				}

				const bool barrierOk =
					g_selfTestBarrierSeen.load(std::memory_order_relaxed);
				const bool heapsOk =
					g_selfTestHeapsSeen.load(std::memory_order_relaxed);
				if (patched && barrierOk && heapsOk) {
					g_selfTestList.store(nullptr, std::memory_order_release);
					g_hookInstallState.store(
						detail::CommandListHookState::Ready, std::memory_order_release);
					REX::INFO("[UiPass] draw hooks armed: "
							   "ID3D12GraphicsCommandList vtable slots {} (barrier) / {} "
							   "(SetDescriptorHeaps) hooked and self-tested",
						kSlotResourceBarrier, kSlotSetDescriptorHeaps);
				} else {
					if (patchedBarrier) {
						(void)PatchSlot(
							vtbl, kSlotResourceBarrier,
							reinterpret_cast<void*>(origBarrier));
					}
					if (patchedHeaps) {
						(void)PatchSlot(
							vtbl, kSlotSetDescriptorHeaps,
							reinterpret_cast<void*>(origHeaps));
					}
					// Keep original targets alive for thunks already in flight after vtable restoration.
					g_selfTestList.store(nullptr, std::memory_order_release);
					MarkDrawHooksFailed();
					REX::ERROR("[UiPass] draw hook self-test FAILED "
							   "(patch b/h={}/{} seen b/h={}/{}); "
							   "vtable restored, UI-pass draw disabled",
						patchedBarrier, patchedHeaps, barrierOk, heapsOk);
				}
				list->Close();
			} else {
				MarkDrawHooksFailed();
				REX::ERROR("[UiPass] draw hooks: throwaway command list creation failed; UI-pass draw disabled");
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


		void RecordOverlayAtHandoff(
			ID3D12GraphicsCommandList* a_list,
			ID3D12Resource* a_buffer,
			const bool a_fgTarget,
			const bool a_regionFirst)
		{
			if (!detail::CanRecordOverlay(
					g_hookInstallState.load(std::memory_order_acquire)) ||
				!g_drawEnabled.load(std::memory_order_acquire) ||
				!a_list || !a_buffer) {
				return;
			}
			const auto target = g_fgTargetPolicy.Observe(a_fgTarget, a_regionFirst);
			if (!target.draw) {
				if (target.frameGeneration && !a_fgTarget &&
					!g_fgLayerOnlyLogged.exchange(true, std::memory_order_relaxed)) {
					REX::DEBUG("[UiPass] under FG only the transparent COPY_SOURCE UI layer is drawn");
				}
				return;
			}

			ID3D12DescriptorHeap* engineHeaps[2]{ tl_heaps[0], tl_heaps[1] };
			const UINT engineHeapCount = tl_heapCount;
			const bool heapKnown =
				engineHeapCount > 0 && tl_heapList == a_list;
			if (!heapKnown) {
				if (!g_unknownEngineHeapLogged.exchange(true, std::memory_order_relaxed)) {
					REX::WARN("[UiPass] overlay draw skipped because the engine descriptor heaps are unknown; preserving engine render state");
				}
				return;
			}

			// Scope compositor-heap suppression so every exit restores engine tracking.
			struct OverlayDrawScope
			{
				OverlayDrawScope() { tl_inOverlayDraw = true; }
				~OverlayDrawScope() { tl_inOverlayDraw = false; }
			};
			const bool drew = [&] {
				const OverlayDrawScope scope;
				return RecordOverlayIntoRenderTarget(a_list, a_buffer, target.firstDrawInRegion);
			}();
			if (!drew) {
				// Early-outs occur before the compositor rebinds descriptor heaps.
				return;
			}
			if (const auto original = g_origSetDescriptorHeaps.load(std::memory_order_relaxed)) {
				original(a_list, engineHeapCount, engineHeaps);
			}
		}

		void* BeginThunk(void* a_this, void* a_ctx, void* a_io, void* a_r9)
		{
			EnsureDrawHooksInstalled();
			// Bound an unfinished post-composite search to the frame that opened it.
			tl_handoffWindow.Cancel();
			const auto original =
				reinterpret_cast<ExecuteFn>(g_origBegin.load(std::memory_order_relaxed));
			return original ? original(a_this, a_ctx, a_io, a_r9) : nullptr;
		}

		void* EndThunk(void* a_this, void* a_ctx, void* a_io, void* a_r9)
		{
			const auto original =
				reinterpret_cast<ExecuteFn>(g_origEnd.load(std::memory_order_relaxed));
			return original ? original(a_this, a_ctx, a_io, a_r9) : nullptr;
		}

		void* CompositeThunk(void* a_this, void* a_ctx, void* a_io, void* a_r9)
		{
			// Capture the engine heaps used by the composite, then draw into the
			// target it hands off. Drawing before this pass makes Starfield apply
			// its fixed-aspect Scaleform transform to an ultrawide browser frame.
			tl_handoffWindow.Begin();
			const auto original =
				reinterpret_cast<ExecuteFn>(
					g_origComposite.load(std::memory_order_relaxed));
			void* result =
				original ? original(a_this, a_ctx, a_io, a_r9) : nullptr;
			tl_handoffWindow.End();
			return result;
		}

		[[nodiscard]] std::uintptr_t HookExecuteSlot(
			const char* a_label,
			const REL::ID a_vtblId,
			const REL::ID a_implId,
			ExecuteFn a_thunk,
			std::atomic<std::uintptr_t>& a_orig)
		{
			const REL::Relocation<std::uintptr_t> vtbl{ a_vtblId };
			const REL::Relocation<std::uintptr_t> expected{ a_implId };
			const auto slotAddress =
				vtbl.address() + kExecuteSlot * sizeof(std::uintptr_t);

			const auto current = *reinterpret_cast<const std::uintptr_t*>(slotAddress);
			if (current != expected.address()) {
				if (!detail::CanChainForeignExecute(current)) {
					REX::WARN("[UiPass] {}: slot 7 is null; nothing to chain, not hooking", a_label);
					return 0;
				}
				const auto owner = Platform::ModuleNameForAddress(reinterpret_cast<const void*>(current));
				REX::INFO("[UiPass] {}: chaining foreign hook from '{}' at 0x{:X} (vanilla implementation was 0x{:X})", a_label, owner.empty() ? "unknown module" : owner, current, expected.address());
			}

			a_orig.store(current, std::memory_order_release);
			if (!REL::WriteSafeData(slotAddress, reinterpret_cast<std::uintptr_t>(a_thunk))) {
				REX::WARN("[UiPass] {}: vtable slot write failed; not hooking", a_label);
				return 0;
			}
			REX::INFO("[UiPass] hooked {} slot 7 "
					   "(vtbl 0x{:X}, original 0x{:X})",
				a_label, vtbl.address(), current);
			return current;
		}

		void RestoreExecuteSlot(
			const char* a_label,
			const REL::ID a_vtblId,
			const std::uintptr_t a_original,
			ExecuteFn a_thunk)
		{
			if (a_original == 0) {
				return;
			}
			const REL::Relocation<std::uintptr_t> vtbl{ a_vtblId };
			const auto slotAddress = vtbl.address() + kExecuteSlot * sizeof(std::uintptr_t);
			const auto current = *reinterpret_cast<const std::uintptr_t*>(slotAddress);
			if (current != reinterpret_cast<std::uintptr_t>(a_thunk)) {
				REX::ERROR("[UiPass] {}: incomplete hook rollback could not verify slot 7; "
						   "leaving the current owner untouched",
					a_label);
				return;
			}
			if (!REL::WriteSafeData(slotAddress, a_original)) {
				REX::ERROR("[UiPass] {}: incomplete hook rollback could not restore slot 7",
					a_label);
				return;
			}
			REX::INFO("[UiPass] restored {} slot 7 after incomplete hook installation", a_label);
		}
	}

	bool Install()
	{
		if (g_installed.exchange(true, std::memory_order_relaxed)) {
			return g_installOk.load(std::memory_order_acquire);
		}

		const auto origBegin = HookExecuteSlot(
			"ScaleformBegin",
			RE::VTABLE::CreationRendererPrivate____ScaleformBeginRenderPass[0],
			RE::ID::CreationRendererPrivate::ScaleformBeginRenderPass::ExecuteRenderPass,
			&BeginThunk, g_origBegin);
		const auto origEnd = HookExecuteSlot(
			"ScaleformEnd",
			RE::VTABLE::CreationRendererPrivate____ScaleformEndRenderPass[0],
			RE::ID::CreationRendererPrivate::ScaleformEndRenderPass::ExecuteRenderPass,
			&EndThunk, g_origEnd);
		const auto origComposite = HookExecuteSlot(
			"ScaleformComposite",
			RE::VTABLE::CreationRendererPrivate__ScaleformCompositeRenderPass[0],
			RE::ID::CreationRendererPrivate::ScaleformCompositeRenderPass::ExecuteRenderPass,
			&CompositeThunk, g_origComposite);

		const bool ok =
			origBegin != 0 && origEnd != 0 && origComposite != 0;
		g_installOk.store(ok, std::memory_order_release);
		g_drawEnabled.store(ok, std::memory_order_release);
		if (!ok) {
			// Install all three entrypoints atomically so their thread-local protocol stays coherent.
			RestoreExecuteSlot("ScaleformBegin",
				RE::VTABLE::CreationRendererPrivate____ScaleformBeginRenderPass[0],
				origBegin, &BeginThunk);
			RestoreExecuteSlot("ScaleformEnd",
				RE::VTABLE::CreationRendererPrivate____ScaleformEndRenderPass[0],
				origEnd, &EndThunk);
			RestoreExecuteSlot("ScaleformComposite",
				RE::VTABLE::CreationRendererPrivate__ScaleformCompositeRenderPass[0],
				origComposite, &CompositeThunk);
			REX::ERROR("[UiPass] hook set incomplete — the overlay has no draw path this "
					   "session. See the per-hook lines above for which slot declined.");
		} else {
			REX::INFO("[UiPass] draw enabled: overlay records into "
					   "Starfield's post-ScaleformComposite target hand-off");
		}
		return ok;
	}

	bool DrawEnabled()
	{
		return g_drawEnabled.load(std::memory_order_acquire);
	}

	bool FrameGenerationActive()
	{
		return g_fgTargetPolicy.FrameGenerationActive();
	}
}
