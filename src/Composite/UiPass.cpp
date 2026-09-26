#include "Composite/UiPass.h"

#include "Composite/D3D12Compositor.h"  // RecordOverlayIntoRenderTarget
#include "Composite/EngineD3D12.h"
#include "Composite/UiPassPolicy.h"
#include "Composite/UiTargetFormat.h"
#include "Core/Log.h"
#include "Platform/WindowsPlatform.h"

#include "Composite/D3D12Prologue.h"  // GDI-free <Windows.h> + <d3d12.h>

#include "RE/C/CreationRenderer.h"
#include "RE/IDs_VTABLE.h"
#include "REL/THook.h"
#include "REL/Utility.h"

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <utility>

namespace OSFUI::UiPass
{
	namespace
	{
		namespace RCP = RE::CreationRendererPrivate;

		constexpr std::size_t kExecuteSlot = 7;

		using ExecuteHook = REL::THookVFT<void*(void*, void*, void*, void*)>;

		std::optional<ExecuteHook> g_beginHook;
		std::optional<ExecuteHook> g_endHook;
		std::optional<ExecuteHook> g_compositeHook;
		std::atomic<bool> g_installed{ false };
		std::atomic_bool g_usePostComposite{ false };
		std::atomic<DXGI_FORMAT> g_postCompositeTargetFormat{ DXGI_FORMAT_UNKNOWN };
		std::atomic_bool g_ignoredPostCompositeFormatLogged{ false };
		std::atomic_bool g_ignoredPostCompositeAspectLogged{ false };
		std::atomic_uint64_t g_expectedOutputSize{ 0 };

		// The ScaleformEnd path draws directly after the native pass and needs no D3D12 hooks. 
		// Only a proven post-composite owner (Luma) still finds its target at the handoff barrier, so only it hooks barriers and heaps.
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

		detail::FrameGenerationTargetPolicy g_fgTargetPolicy;
		std::atomic_bool g_fgLayerOnlyLogged{ false };

		void RecordOverlayAtHandoff(ID3D12GraphicsCommandList* a_list, ID3D12Resource* a_buffer, bool a_fgTarget, bool a_regionFirst);
		std::atomic<ID3D12GraphicsCommandList*> g_selfTestList{ nullptr };
		std::atomic<bool> g_selfTestBarrierSeen{ false };
		std::atomic<bool> g_selfTestHeapsSeen{ false };
		std::atomic_bool g_unknownEngineHeapLogged{ false };

		// Direct ScaleformEnd draw. Begin and End of one UI subgraph run in sequence on the same render worker, so the copied handle is per thread.
		thread_local std::optional<RCP::GraphResourceHandle> tl_beginTarget;
		std::atomic_bool g_directTargetRejectedLogged{ false };

		[[nodiscard]] bool IsUiRenderTarget(const D3D12_RESOURCE_DESC& a_desc)
		{
			return UiTargetFormat::ResolveRtv(a_desc.Format) != DXGI_FORMAT_UNKNOWN &&
				a_desc.Dimension == D3D12_RESOURCE_DIMENSION_TEXTURE2D &&
				(a_desc.Flags & D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET) &&
				a_desc.SampleDesc.Count == 1 && a_desc.DepthOrArraySize == 1 &&
				a_desc.Width >= 256 && a_desc.Height >= 256;
		}

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

		void STDMETHODCALLTYPE ResourceBarrierThunk(ID3D12GraphicsCommandList* a_self, const UINT a_numBarriers, const D3D12_RESOURCE_BARRIER* a_barriers) noexcept
		{
			tl_handoffWindow.OnBarrierCall();
			if (tl_handoffWindow.HandoffArmed() && a_barriers) {
				for (UINT i = 0; i < a_numBarriers && tl_handoffWindow.HandoffArmed(); ++i) {
					const auto& barrier = a_barriers[i];
					if (barrier.Type != D3D12_RESOURCE_BARRIER_TYPE_TRANSITION ||
						(barrier.Flags & D3D12_RESOURCE_BARRIER_FLAG_END_ONLY) ||
						!barrier.Transition.pResource ||
						(barrier.Transition.Subresource != D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES &&
							barrier.Transition.Subresource != 0) ||
						barrier.Transition.StateBefore != D3D12_RESOURCE_STATE_RENDER_TARGET ||
						!(barrier.Transition.StateAfter &
							(D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE | D3D12_RESOURCE_STATE_COPY_SOURCE))) {
						continue;
					}
					const auto desc = barrier.Transition.pResource->GetDesc();
					if (!IsUiRenderTarget(desc)) {
						continue;
					}
					const auto rtvFormat = UiTargetFormat::ResolveRtv(desc.Format);
					if (rtvFormat != g_postCompositeTargetFormat.load(std::memory_order_acquire)) {
						if (!g_ignoredPostCompositeFormatLogged.exchange(true, std::memory_order_relaxed)) {
							REX::DEBUG("[UiPass] ignored non-UI post-composite target {}x{} {}; waiting for {}",
								static_cast<std::uint64_t>(desc.Width), desc.Height,
								UiTargetFormat::Name(rtvFormat),
								UiTargetFormat::Name(g_postCompositeTargetFormat.load(std::memory_order_relaxed)));
						}
						continue;
					}
					if (a_self->GetType() != D3D12_COMMAND_LIST_TYPE_DIRECT) {
						continue;
					}
					const auto expectedOutput = g_expectedOutputSize.load(std::memory_order_acquire);
					const auto expectedWidth = static_cast<std::uint32_t>(expectedOutput >> 32);
					const auto expectedHeight = static_cast<std::uint32_t>(expectedOutput);
					const bool targetAspectMatchesOutput = detail::PostCompositeTargetMatchesOutputAspect(
						desc.Width, desc.Height, expectedWidth, expectedHeight);
					// The post-composite output must match the swapchain aspect ratio.
					if (!targetAspectMatchesOutput) {
						if (!g_ignoredPostCompositeAspectLogged.exchange(true, std::memory_order_relaxed)) {
							REX::DEBUG("[UiPass] ignored post-composite target {}x{} with aspect incompatible with output {}x{}; continuing hand-off search",
								static_cast<std::uint64_t>(desc.Width), desc.Height,
								expectedWidth, expectedHeight);
						}
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
				!g_installed.load(std::memory_order_acquire) ||
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

		// Runs after the native ScaleformEnd returns, while the UI texture is still
		// in RENDER_TARGET and before the graph transitions it for its consumer.
		// The native reset restores heaps and invalidates cached bindings for the
		// next native state application. The consumer must still bind its own
		// targets, viewport and scissor.
		void RecordOverlayAfterEnd(void* a_graph, void* a_io)
		{
			const auto handle = std::exchange(tl_beginTarget, std::nullopt);
			if (!handle || !g_installed.load(std::memory_order_acquire)) {
				return;
			}

			// End's own IO entry is not the UI texture: resolve Begin's handle
			// against End's current pool.
			auto* context = static_cast<RCP::GraphContext*>(a_graph)->GetOpenCommandContext();
			auto* pool = static_cast<RCP::PassIO*>(a_io)->pool;
			if (!context || !context->commandList || !context->commandPool || !pool) {
				return;
			}
			// ID 142968 only rebinds heaps for these command-pool flags. Skip before
			// touching the list if its reset would leave our descriptor heap bound.
			const auto poolFlags = *(static_cast<const std::uint8_t*>(context->commandPool) + 0x20);
			if ((poolFlags & 0xFD) != 0) {
				return;
			}
			const auto* renderTarget = static_cast<RCP::GraphRenderTarget*>(pool->Resolve(*handle));
			if (!renderTarget || !renderTarget->texture || !renderTarget->texture->resource) {
				return;
			}
			auto* list = reinterpret_cast<ID3D12GraphicsCommandList*>(context->commandList);
			auto* resource = reinterpret_cast<ID3D12Resource*>(renderTarget->texture->resource);
			if (const auto desc = resource->GetDesc(); !IsUiRenderTarget(desc)) {
				if (!g_directTargetRejectedLogged.exchange(true, std::memory_order_relaxed)) {
					REX::WARN("[UiPass] skipped ScaleformEnd target {}x{} (format {}, dimension {}, flags 0x{:X}); not a drawable UI layer",
						static_cast<std::uint64_t>(desc.Width), desc.Height, static_cast<unsigned>(desc.Format),
						static_cast<unsigned>(desc.Dimension), static_cast<unsigned>(desc.Flags));
				}
				return;
			}

			// Each End closes one UI subgraph, so every draw starts a region.
			if (!RecordOverlayIntoRenderTarget(list, resource, true)) {
				// Early-outs occur before the compositor rebinds descriptor heaps.
				return;
			}
			context->InvalidateCachedBindingsAndBindHeaps();
		}

		void* BeginThunk(void* a_this, void* a_ctx, void* a_io, void* a_r9)
		{
			const bool postComposite = g_usePostComposite.load(std::memory_order_acquire);
			if (postComposite) {
				EnsureDrawHooksInstalled();
				// Bound an unfinished post-composite search to the frame that opened it.
				tl_handoffWindow.Cancel();
			} else {
				tl_beginTarget.reset();
			}
			auto* result = (*g_beginHook)(a_this, a_ctx, a_io, a_r9);
			if (!postComposite) {
				if (const auto* entries = static_cast<RCP::PassIO*>(a_io)->entries) {
					if (const auto io = entries->GetEntries(); !io.empty()) {
						tl_beginTarget = io.front().handle;
					}
				}
			}
			return result;
		}

		void* EndThunk(void* a_this, void* a_ctx, void* a_io, void* a_r9)
		{
			void* result = (*g_endHook)(a_this, a_ctx, a_io, a_r9);
			if (!g_usePostComposite.load(std::memory_order_acquire)) {
				RecordOverlayAfterEnd(a_ctx, a_io);
			}
			return result;
		}

		void* CompositeThunk(void* a_this, void* a_ctx, void* a_io, void* a_r9)
		{
			const bool postComposite = g_usePostComposite.load(std::memory_order_acquire);
			if (postComposite) {
				// Capture the engine heaps used by the composite, then draw into its
				// validated UI target after the fixed-aspect transform.
				tl_handoffWindow.Begin();
			}
			void* result = (*g_compositeHook)(a_this, a_ctx, a_io, a_r9);
			if (postComposite) {
				tl_handoffWindow.End();
			}
			return result;
		}

		void ConfigureDrawPath()
		{
			const REL::Relocation<std::uintptr_t> compositeVtable{
				RE::VTABLE::CreationRendererPrivate__ScaleformCompositeRenderPass[0]
			};
			const auto current = reinterpret_cast<const std::uintptr_t*>(compositeVtable.address())[kExecuteSlot];
			const bool vanilla = current == RE::ID::CreationRendererPrivate::ScaleformCompositeRenderPass::ExecuteRenderPass.address();
			const auto owner = Platform::ModuleNameForAddress(reinterpret_cast<const void*>(current));
			const auto target = detail::SelectPostCompositeTarget(vanilla, owner);
			g_usePostComposite.store(target.supported, std::memory_order_release);
			g_postCompositeTargetFormat.store(
				target.format == detail::PostCompositeTargetFormat::Rgba16Float ?
					DXGI_FORMAT_R16G16B16A16_FLOAT : DXGI_FORMAT_R8G8B8A8_UNORM,
				std::memory_order_release);
			REX::INFO("[UiPass] draw path: {} (composite owner: {})",
				target.supported ? "post-ScaleformComposite" : "ScaleformEnd",
				owner.empty() ? "unknown" : owner);
		}
	}

	bool Install()
	{
		if (g_installed.load(std::memory_order_acquire)) {
			return true;
		}

		// Inspect the existing composite owner before replacing its slot.
		ConfigureDrawPath();
		g_beginHook.emplace("UiPass::ScaleformBegin",
			RE::VTABLE::CreationRendererPrivate____ScaleformBeginRenderPass[0],
			kExecuteSlot, &BeginThunk);
		g_endHook.emplace("UiPass::ScaleformEnd",
			RE::VTABLE::CreationRendererPrivate____ScaleformEndRenderPass[0],
			kExecuteSlot, &EndThunk);
		g_compositeHook.emplace("UiPass::ScaleformComposite",
			RE::VTABLE::CreationRendererPrivate__ScaleformCompositeRenderPass[0],
			kExecuteSlot, &CompositeThunk);
		g_beginHook->Enable();
		g_endHook->Enable();
		g_compositeHook->Enable();
		g_installed.store(true, std::memory_order_release);
		return true;
	}

	bool DrawEnabled()
	{
		if (!g_installed.load(std::memory_order_acquire)) {
			return false;
		}
		return !g_usePostComposite.load(std::memory_order_acquire) ||
			g_hookInstallState.load(std::memory_order_acquire) != detail::CommandListHookState::Failed;
	}

	bool UsesScaleformEnd()
	{
		return !g_usePostComposite.load(std::memory_order_acquire);
	}

	void SetExpectedOutputSize(const std::uint32_t a_width, const std::uint32_t a_height)
	{
		const auto packed = (static_cast<std::uint64_t>(a_width) << 32) | a_height;
		g_expectedOutputSize.store(packed, std::memory_order_release);
	}
}
