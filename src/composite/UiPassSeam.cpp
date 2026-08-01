#include "composite/UiPassSeam.h"

#include "composite/D3D12Compositor.h"  // RecordSeamOverlayDraw (real-overlay seam draw)
#include "composite/EngineD3D12.h"
#include "composite/SeamTargetFormat.h"
#include "composite/UiPassSeamPolicy.h"
#if defined(OSFUI_WITH_WORLD_SURFACES)
#include "composite/WorldSurface.h"
#endif
#include "core/Log.h"
#include "platform/WindowsPlatform.h"

#include "composite/D3D12Prologue.h"  // GDI-free <Windows.h> + <d3d12.h>

#include <atomic>
#include <cstddef>
#include <cstdint>

namespace OSFUI::UiPassSeam
{
	namespace
	{
		// AddrLib IDs, proven on 1.16.244. Canonical record with disassembly
		// evidence: OSF RE context module `rendering.ui_pass` (2026-07-21).
		constexpr std::uint64_t kVtblScaleformBegin = 497423;
		constexpr std::uint64_t kVtblScaleformEnd = 497425;
		constexpr std::uint64_t kVtblScaleformComposite = 497272;
		constexpr std::uint64_t kIdBeginExecute = 145955;
		constexpr std::uint64_t kIdEndExecute = 145956;
		constexpr std::uint64_t kIdCompositeExecute = 145827;

		constexpr std::size_t kExecuteSlot = 7;

		using ExecuteFn = void* (*)(void*, void*, void*, void*);

		std::atomic<std::uintptr_t> g_origBegin{ 0 };
		std::atomic<std::uintptr_t> g_origEnd{ 0 };
		std::atomic<std::uintptr_t> g_origComposite{ 0 };
		std::atomic<bool> g_installed{ false };
		std::atomic<bool> g_installOk{ false };

		// -------------------------------------------------- D3D12 seam hooks
		// This is the known-good pre-b8e3643 implementation. It hooks only the
		// hand-off barrier and descriptor heaps; root-signature/PSO interception
		// is intentionally absent.
		constexpr std::size_t kSlotResourceBarrier = 26;
		constexpr std::size_t kSlotSetDescriptorHeaps = 28;

		using ResourceBarrierFn = void(STDMETHODCALLTYPE*)(
			ID3D12GraphicsCommandList*, UINT, const D3D12_RESOURCE_BARRIER*);
		using SetDescriptorHeapsFn = void(STDMETHODCALLTYPE*)(
			ID3D12GraphicsCommandList*, UINT, ID3D12DescriptorHeap* const*);

		std::atomic<ResourceBarrierFn> g_origResourceBarrier{ nullptr };
		std::atomic<SetDescriptorHeapsFn> g_origSetDescriptorHeaps{ nullptr };

		// The last SetDescriptorHeaps the ENGINE issued on this thread's command
		// list, so the seam draw can put it back after binding its own heap.
		// "Engine" is load-bearing: see tl_inSeamDraw.
		thread_local ID3D12GraphicsCommandList* tl_heapList = nullptr;
		thread_local ID3D12DescriptorHeap* tl_heaps[2] = {};
		thread_local UINT tl_heapCount = 0;

		// True while RecordSeamOverlayDraw is recording our overlay onto the
		// engine's list. The compositor binds its own SRV heap through the
		// *virtual* ID3D12GraphicsCommandList::SetDescriptorHeaps, and that vtable
		// is the one patched below — so without this flag the tracker above would
		// record OUR heap as if the engine had bound it. The restore that follows
		// calls the original directly and therefore never corrects it, leaving
		// {srvHeap}, count=1 latched. A second qualifying hand-off in the same
		// ScaleformEnd region (tl_handoffDrawsLeft starts at 2) would then
		// "restore" our 9-descriptor SRV heap onto the engine's list and drop its
		// sampler heap, so the engine's next root-descriptor-table resolve reads
		// our heap and any sampler table hits an unbound one.
		thread_local bool tl_inSeamDraw = false;
		std::atomic<int> g_hookInstallState{ 0 };
		// Gates RecordSeamDrawAtHandoff. Set from Install() and cleared by
		// EnsureDrawHooksInstalled on any failure path — declared up here (rather
		// than beside its first reader) precisely so that lazy installer, which
		// runs long after Install() returned true, can turn it back off.
		std::atomic<bool> g_drawEnabled{ false };

		thread_local int tl_handoffDrawsLeft = 0;
		thread_local int tl_callsAfterFirstDraw = -1;
		void RecordSeamDrawAtHandoff(ID3D12GraphicsCommandList* a_list, ID3D12Resource* a_buffer,
			bool a_fgTarget, bool a_regionFirst);
		ID3D12GraphicsCommandList* g_selfTestList = nullptr;
		std::atomic<bool> g_selfTestBarrierSeen{ false };
		std::atomic<bool> g_selfTestHeapsSeen{ false };

		void STDMETHODCALLTYPE SetDescriptorHeapsThunk(
			ID3D12GraphicsCommandList* a_self,
			const UINT a_num,
			ID3D12DescriptorHeap* const* a_heaps)
		{
			if (a_self == g_selfTestList) {
				g_selfTestHeapsSeen.store(true, std::memory_order_relaxed);
			} else if (!tl_inSeamDraw) {
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
			if (tl_handoffDrawsLeft > 0 && tl_callsAfterFirstDraw >= 0 &&
				++tl_callsAfterFirstDraw > 4) {
				tl_handoffDrawsLeft = 0;
				tl_callsAfterFirstDraw = -1;
			}
			if (tl_handoffDrawsLeft > 0 && a_barriers) {
				for (UINT i = 0; i < a_numBarriers && tl_handoffDrawsLeft > 0; ++i) {
					const auto& barrier = a_barriers[i];
					if (barrier.Type != D3D12_RESOURCE_BARRIER_TYPE_TRANSITION ||
						!barrier.Transition.pResource ||
						barrier.Transition.StateBefore != D3D12_RESOURCE_STATE_RENDER_TARGET ||
						!(barrier.Transition.StateAfter &
							(D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE |
								D3D12_RESOURCE_STATE_COPY_SOURCE))) {
						continue;
					}
					const auto desc = barrier.Transition.pResource->GetDesc();
					if (SeamTargetFormat::ResolveRtv(desc.Format) == DXGI_FORMAT_UNKNOWN ||
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
					const bool regionFirst = tl_handoffDrawsLeft == 2;
					--tl_handoffDrawsLeft;
					if (tl_callsAfterFirstDraw < 0) {
						tl_callsAfterFirstDraw = 0;
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

		void EnsureDrawHooksInstalled()
		{
			int expected = 0;
			if (!g_hookInstallState.compare_exchange_strong(
					expected, -1, std::memory_order_acq_rel)) {
				return;
			}

			const auto engine = LocateEngineD3D12();
			if (!engine) {
				g_drawEnabled.store(false, std::memory_order_release);
				REX::ERROR("[UiPassSeam] draw hooks: engine D3D12 device not reachable; seam draw disabled");
				return;
			}

			ID3D12CommandAllocator* allocator = nullptr;
			ID3D12GraphicsCommandList* list = nullptr;
			auto* device = reinterpret_cast<ID3D12Device*>(engine.device);
#if defined(OSFUI_WITH_WORLD_SURFACES)
			WorldSurface::TryInstall(device);
#endif
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
				const auto origBarrier =
					reinterpret_cast<ResourceBarrierFn>(vtbl[kSlotResourceBarrier]);
				const auto origHeaps =
					reinterpret_cast<SetDescriptorHeapsFn>(vtbl[kSlotSetDescriptorHeaps]);
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
					g_selfTestList = nullptr;
					g_hookInstallState.store(1, std::memory_order_release);
					REX::INFO("[UiPassSeam] seam draw hooks armed: "
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
					// Deliberately NOT nulling g_origResourceBarrier /
					// g_origSetDescriptorHeaps here. The vtable restore above
					// already stops new entries; a thunk call that is already past
					// its entry check still needs the original to forward to, and
					// dropping a ResourceBarrier on the floor would leave a
					// resource in the wrong state. Nothing treats these pointers
					// as an "installed" flag — g_hookInstallState and
					// g_drawEnabled do that.
					g_selfTestList = nullptr;
					// The log said "seam draw disabled" while leaving it enabled,
					// so the compositor kept seam mode on with nothing able to
					// draw: an invisible overlay reported as healthy.
					g_drawEnabled.store(false, std::memory_order_release);
					REX::ERROR("[UiPassSeam] seam draw hook self-test FAILED "
							   "(patch b/h={}/{} seen b/h={}/{}); "
							   "vtable restored, seam draw disabled",
						patchedBarrier, patchedHeaps, barrierOk, heapsOk);
				}
				list->Close();
			} else {
				g_drawEnabled.store(false, std::memory_order_release);
				REX::ERROR("[UiPassSeam] draw hooks: throwaway command list creation failed; seam draw disabled");
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


		void RecordSeamDrawAtHandoff(
			ID3D12GraphicsCommandList* a_list,
			ID3D12Resource* a_buffer,
			const bool a_fgTarget,
			const bool a_regionFirst)
		{
			if (!g_drawEnabled.load(std::memory_order_relaxed) ||
				!a_list || !a_buffer) {
				return;
			}

			ID3D12DescriptorHeap* engineHeaps[2]{ tl_heaps[0], tl_heaps[1] };
			const UINT engineHeapCount = tl_heapCount;
			const bool heapKnown =
				engineHeapCount > 0 && tl_heapList == a_list;

			// Suppress heap tracking for the duration of our own recording, so
			// the snapshot above still describes the engine's binding when the
			// next hand-off in this region reads it. Scoped rather than a bare
			// pair of assignments because every exit from the draw has to clear
			// it — a stuck flag would make the tracker miss the engine's next
			// real bind, which is the same corruption one step removed.
			struct SeamDrawScope
			{
				SeamDrawScope() { tl_inSeamDraw = true; }
				~SeamDrawScope() { tl_inSeamDraw = false; }
			};
			const bool drew = [&] {
				const SeamDrawScope scope;
				return RecordSeamOverlayDraw(a_list, a_buffer, a_fgTarget, a_regionFirst);
			}();
			if (!drew) {
				// Every early-out in RecordSeamOverlayDraw precedes its
				// SetDescriptorHeaps, so nothing was rebound and there is
				// nothing to put back.
				return;
			}
			if (heapKnown) {
				if (const auto original =
						g_origSetDescriptorHeaps.load(std::memory_order_relaxed)) {
					original(a_list, engineHeapCount, engineHeaps);
				}
			}
		}

		void* BeginThunk(void* a_this, void* a_ctx, void* a_io, void* a_r9)
		{
			EnsureDrawHooksInstalled();
			const auto original =
				reinterpret_cast<ExecuteFn>(g_origBegin.load(std::memory_order_relaxed));
			return original ? original(a_this, a_ctx, a_io, a_r9) : nullptr;
		}

		void* EndThunk(void* a_this, void* a_ctx, void* a_io, void* a_r9)
		{
			const auto original =
				reinterpret_cast<ExecuteFn>(g_origEnd.load(std::memory_order_relaxed));
			void* result =
				original ? original(a_this, a_ctx, a_io, a_r9) : nullptr;
			tl_handoffDrawsLeft = 2;
			tl_callsAfterFirstDraw = -1;
			return result;
		}

		void* CompositeThunk(void* a_this, void* a_ctx, void* a_io, void* a_r9)
		{
			tl_handoffDrawsLeft = 0;
			tl_callsAfterFirstDraw = -1;
			const auto original =
				reinterpret_cast<ExecuteFn>(
					g_origComposite.load(std::memory_order_relaxed));
			return original ? original(a_this, a_ctx, a_io, a_r9) : nullptr;
		}

		[[nodiscard]] std::uintptr_t HookExecuteSlot(
			const char* a_label,
			const detail::ExecuteSlotKind a_kind,
			const std::uint64_t a_vtblId,
			const std::uint64_t a_implId,
			ExecuteFn a_thunk,
			std::atomic<std::uintptr_t>& a_orig)
		{
			const REL::Relocation<std::uintptr_t> vtbl{ REL::ID(a_vtblId) };
			const REL::Relocation<std::uintptr_t> expected{ REL::ID(a_implId) };
			const auto slotAddress =
				vtbl.address() + kExecuteSlot * sizeof(std::uintptr_t);

			std::uintptr_t current = 0;
			if (!Platform::SafeReadPointer(slotAddress, current)) {
				REX::WARN("[UiPassSeam] {}: vtable slot at 0x{:X} unreadable; not hooking",
					a_label, slotAddress);
				return 0;
			}
			if (current != expected.address()) {
				const auto owner = Platform::ModuleNameForAddress(
					reinterpret_cast<const void*>(current));
				if (!detail::CanChainForeignExecute(a_kind, owner)) {
					REX::WARN("[UiPassSeam] {}: slot 7 holds 0x{:X} from '{}', expected "
							  "0x{:X} (game patch or unsupported foreign hook); not hooking",
						a_label, current, owner.empty() ? "unknown module" : owner,
						expected.address());
					return 0;
				}
				REX::INFO("[UiPassSeam] {}: chaining compatible hook from '{}' at 0x{:X}",
					a_label, owner, current);
			}

			auto** slot = reinterpret_cast<void**>(slotAddress);
			DWORD oldProtect = 0;
			if (!::VirtualProtect(slot, sizeof(void*), PAGE_READWRITE, &oldProtect)) {
				REX::WARN("[UiPassSeam] {}: VirtualProtect failed; not hooking", a_label);
				return 0;
			}
			a_orig.store(current, std::memory_order_release);
			*slot = reinterpret_cast<void*>(a_thunk);
			::VirtualProtect(slot, sizeof(void*), oldProtect, &oldProtect);
			REX::INFO("[UiPassSeam] hooked {} slot 7 "
					   "(vtbl 0x{:X}, original 0x{:X})",
				a_label, vtbl.address(), current);
			return current;
		}

		void RestoreExecuteSlot(
			const char* a_label,
			const std::uint64_t a_vtblId,
			const std::uintptr_t a_original,
			ExecuteFn a_thunk)
		{
			if (a_original == 0) {
				return;
			}
			const REL::Relocation<std::uintptr_t> vtbl{ REL::ID(a_vtblId) };
			const auto slotAddress = vtbl.address() + kExecuteSlot * sizeof(std::uintptr_t);
			std::uintptr_t current = 0;
			if (!Platform::SafeReadPointer(slotAddress, current) ||
				current != reinterpret_cast<std::uintptr_t>(a_thunk)) {
				REX::ERROR("[UiPassSeam] {}: incomplete hook rollback could not verify slot 7; "
						   "leaving the current owner untouched",
					a_label);
				return;
			}
			auto** slot = reinterpret_cast<void**>(slotAddress);
			DWORD oldProtect = 0;
			if (!::VirtualProtect(slot, sizeof(void*), PAGE_READWRITE, &oldProtect)) {
				REX::ERROR("[UiPassSeam] {}: incomplete hook rollback could not make slot 7 writable",
					a_label);
				return;
			}
			*slot = reinterpret_cast<void*>(a_original);
			DWORD ignored = 0;
			if (!::VirtualProtect(slot, sizeof(void*), oldProtect, &ignored)) {
				REX::WARN("[UiPassSeam] {}: slot 7 restored but its page protection was not", a_label);
			}
			REX::INFO("[UiPassSeam] restored {} slot 7 after incomplete hook installation", a_label);
		}
	}

	bool Install()
	{
		if (g_installed.exchange(true, std::memory_order_relaxed)) {
			return g_installOk.load(std::memory_order_acquire);
		}

		const auto origBegin = HookExecuteSlot(
			"ScaleformBegin", detail::ExecuteSlotKind::Begin,
			kVtblScaleformBegin, kIdBeginExecute,
			&BeginThunk, g_origBegin);
		const auto origEnd = HookExecuteSlot(
			"ScaleformEnd", detail::ExecuteSlotKind::End,
			kVtblScaleformEnd, kIdEndExecute,
			&EndThunk, g_origEnd);
		const auto origComposite = HookExecuteSlot(
			"ScaleformComposite", detail::ExecuteSlotKind::Composite,
			kVtblScaleformComposite, kIdCompositeExecute,
			&CompositeThunk, g_origComposite);

		const bool ok =
			origBegin != 0 && origEnd != 0 && origComposite != 0;
		g_installOk.store(ok, std::memory_order_release);
		g_drawEnabled.store(ok, std::memory_order_release);
		if (!ok) {
			// The three entrypoints form one protocol. Leaving only a subset
			// patched lets their thread-local hand-off state drift indefinitely.
			RestoreExecuteSlot("ScaleformBegin", kVtblScaleformBegin, origBegin, &BeginThunk);
			RestoreExecuteSlot("ScaleformEnd", kVtblScaleformEnd, origEnd, &EndThunk);
			RestoreExecuteSlot("ScaleformComposite", kVtblScaleformComposite,
				origComposite, &CompositeThunk);
			REX::ERROR("[UiPassSeam] hook set incomplete — the overlay has no draw path this "
					   "session. See the per-hook lines above for which slot declined.");
		} else {
			REX::INFO("[UiPassSeam] seam draw enabled: overlay records into "
					   "Starfield's transparent UI layer at the ScaleformEnd hand-off");
		}
		return ok;
	}
}
