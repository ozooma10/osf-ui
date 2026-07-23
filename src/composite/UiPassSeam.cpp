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

		thread_local ID3D12GraphicsCommandList* tl_heapList = nullptr;
		thread_local ID3D12DescriptorHeap* tl_heaps[2] = {};
		thread_local UINT tl_heapCount = 0;
		std::atomic<int> g_hookInstallState{ 0 };

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
				REX::ERROR("[UiPassSeam] draw hooks: engine D3D12 device not reachable; seam draw disabled");
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
					REX::DEBUG("[UiPassSeam] seam draw hooks armed: "
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
					g_origResourceBarrier.store(nullptr, std::memory_order_relaxed);
					g_origSetDescriptorHeaps.store(nullptr, std::memory_order_relaxed);
					g_selfTestList = nullptr;
					REX::WARN("[UiPassSeam] seam draw hook self-test FAILED "
							  "(patch b/h={}/{} seen b/h={}/{}); "
							  "vtable restored, seam draw disabled",
						patchedBarrier, patchedHeaps, barrierOk, heapsOk);
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

		std::atomic<bool> g_drawEnabled{ false };

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

			if (!RecordSeamOverlayDraw(
					a_list, a_buffer, a_fgTarget, a_regionFirst)) {
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
				REX::WARN("[UiPassSeam] {}: slot 7 holds 0x{:X}, expected 0x{:X} "
						  "(game patch or foreign hook); not hooking",
					a_label, current, expected.address());
				return 0;
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
			REX::DEBUG("[UiPassSeam] hooked {} slot 7 "
					   "(vtbl 0x{:X}, original 0x{:X})",
				a_label, vtbl.address(), current);
			return current;
		}
	}

	bool Install()
	{
		if (g_installed.exchange(true, std::memory_order_relaxed)) {
			return g_installOk.load(std::memory_order_acquire);
		}

		const auto origBegin = HookExecuteSlot(
			"ScaleformBegin", kVtblScaleformBegin, kIdBeginExecute,
			&BeginThunk, g_origBegin);
		const auto origEnd = HookExecuteSlot(
			"ScaleformEnd", kVtblScaleformEnd, kIdEndExecute,
			&EndThunk, g_origEnd);
		const auto origComposite = HookExecuteSlot(
			"ScaleformComposite", kVtblScaleformComposite, kIdCompositeExecute,
			&CompositeThunk, g_origComposite);

		const bool ok =
			origBegin != 0 && origEnd != 0 && origComposite != 0;
		g_installOk.store(ok, std::memory_order_release);
		g_drawEnabled.store(ok, std::memory_order_release);
		if (!ok) {
			REX::ERROR("[UiPassSeam] hook set incomplete — the overlay has no draw path this "
					   "session. See the per-hook lines above for which slot declined.");
		} else {
			REX::DEBUG("[UiPassSeam] seam draw enabled: overlay records into "
					   "Starfield's transparent UI layer at the ScaleformEnd hand-off");
		}
		return ok;
	}
}
