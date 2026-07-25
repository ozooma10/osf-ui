#include "composite/WorldSurface.h"

#include "composite/D3D12Prologue.h"
#include "platform/WindowsPlatform.h"

#include <atomic>
#include <mutex>

namespace OSFUI::WorldSurface
{
	namespace
	{
		constexpr std::size_t kCreateSrvSlot = 18;

		using CreateSrvFn = void(STDMETHODCALLTYPE*)(
			ID3D12Device*, ID3D12Resource*,
			const D3D12_SHADER_RESOURCE_VIEW_DESC*,
			D3D12_CPU_DESCRIPTOR_HANDLE);

		std::atomic_bool g_enabled{ false };
		std::atomic_bool g_installTried{ false };
		std::atomic_bool g_installed{ false };
		std::atomic<CreateSrvFn> g_original{ nullptr };
		std::atomic<std::uint32_t> g_targetWidth{ 0 };
		std::atomic<std::uint32_t> g_targetHeight{ 0 };

		std::mutex g_mutex;
		ID3D12Device* g_device{ nullptr };
		D3D12_CPU_DESCRIPTOR_HANDLE g_targetSrv{};
		ID3D12Resource* g_slots[SharedRingDesc::kMaxSlots]{};
		std::uint32_t g_slotCount{ 0 };
		ID3D12Fence* g_produceFence{ nullptr };
		ID3D12Fence* g_consumeFence{ nullptr };
		SharedRingDesc g_pending{};
		bool g_pendingDirty{ false };
		std::uint64_t g_generation{ 0 };
		std::uint64_t g_lastSerial{ 0 };
		std::uint32_t g_lastSlot{ 0 };
		std::uint64_t g_pendingConsumeSerial{ 0 };
		bool          g_loggedWrite{ false };
		bool          g_loggedConsume{ false };
		std::uint32_t g_fenceStalls{ 0 };
		std::uint64_t g_refreshWrites{ 0 };
		std::uint64_t g_nextRefreshLog{ 1 };

		template <class T>
		void SafeRelease(T*& a_value)
		{
			if (a_value) {
				a_value->Release();
				a_value = nullptr;
			}
		}

		void CloseHandles(SharedRingDesc& a_desc)
		{
			for (auto*& handle : a_desc.slotHandles) {
				if (handle) {
					::CloseHandle(handle);
					handle = nullptr;
				}
			}
			if (a_desc.produceFence) {
				::CloseHandle(a_desc.produceFence);
				a_desc.produceFence = nullptr;
			}
			if (a_desc.consumeFence) {
				::CloseHandle(a_desc.consumeFence);
				a_desc.consumeFence = nullptr;
			}
		}

		void ReleaseRing()
		{
			for (auto*& slot : g_slots) {
				SafeRelease(slot);
			}
			SafeRelease(g_produceFence);
			SafeRelease(g_consumeFence);
			g_slotCount = 0;
			g_lastSerial = 0;
			g_lastSlot = 0;
			g_pendingConsumeSerial = 0;
			g_fenceStalls = 0;
			g_loggedWrite = false;
			g_loggedConsume = false;
		}

		[[nodiscard]] bool IsTarget(ID3D12Resource* a_resource)
		{
			if (!a_resource) {
				return false;
			}
			const auto desc = a_resource->GetDesc();
			return desc.Dimension == D3D12_RESOURCE_DIMENSION_TEXTURE2D &&
				desc.Width == g_targetWidth.load(std::memory_order_relaxed) &&
				desc.Height == g_targetHeight.load(std::memory_order_relaxed);
		}

		// Returns false when the material descriptor has not been captured yet, so
		// callers never latch "displayed" state for a write that did not land.
		bool WriteReplacement(ID3D12Resource* a_resource)
		{
			const auto original = g_original.load(std::memory_order_acquire);
			if (!original || !g_device || !a_resource || g_targetSrv.ptr == 0) {
				return false;
			}
			D3D12_SHADER_RESOURCE_VIEW_DESC srv{};
			srv.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
			srv.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
			srv.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
			srv.Texture2D.MipLevels = 1;
			original(g_device, a_resource, &srv, g_targetSrv);
			return true;
		}

		void STDMETHODCALLTYPE CreateSrvThunk(
			ID3D12Device* a_device,
			ID3D12Resource* a_resource,
			const D3D12_SHADER_RESOURCE_VIEW_DESC* a_desc,
			D3D12_CPU_DESCRIPTOR_HANDLE a_destination)
		{
			const auto original = g_original.load(std::memory_order_acquire);
			if (!original) {
				return;
			}
			if (!IsTarget(a_resource)) {
				original(a_device, a_resource, a_desc, a_destination);
				return;
			}

			std::scoped_lock lock(g_mutex);
			g_targetSrv = a_destination;
			const bool replaced = g_slotCount != 0 && g_slots[g_lastSlot] != nullptr;
			if (replaced) {
				D3D12_SHADER_RESOURCE_VIEW_DESC srv{};
				srv.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
				srv.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
				srv.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
				srv.Texture2D.MipLevels = 1;
				original(a_device, g_slots[g_lastSlot], &srv, a_destination);
			} else {
				original(a_device, a_resource, a_desc, a_destination);
			}
			// Every capture is logged: a second line for the same material means the
			// engine re-created the descriptor (streaming/heap rebuild), which is the
			// prime suspect whenever a landed write stops being visible.
			const auto resource = a_resource->GetDesc();
			REX::INFO("[WorldSurface] captured placeholder {}x{} at srvCpu=0x{:X} "
				"(resFormat {}, mips {}, viewFormat {}, replaced={})",
				g_targetWidth.load(), g_targetHeight.load(), a_destination.ptr,
				static_cast<int>(resource.Format), resource.MipLevels,
				a_desc ? static_cast<int>(a_desc->Format) : -1, replaced);
		}

		bool AdoptPending()
		{
			if (!g_pendingDirty || !g_device) {
				return g_slotCount != 0;
			}
			auto pending = g_pending;
			g_pending = {};
			g_pendingDirty = false;
			ReleaseRing();

			bool ok = pending.slotCount > 0 &&
				pending.slotCount <= SharedRingDesc::kMaxSlots;
			for (std::uint32_t i = 0; ok && i < pending.slotCount; ++i) {
				ok = pending.slotHandles[i] &&
					SUCCEEDED(g_device->OpenSharedHandle(
						pending.slotHandles[i], __uuidof(ID3D12Resource),
						reinterpret_cast<void**>(&g_slots[i])));
			}
			if (ok) {
				ok = pending.produceFence &&
					SUCCEEDED(g_device->OpenSharedHandle(
						pending.produceFence, __uuidof(ID3D12Fence),
						reinterpret_cast<void**>(&g_produceFence)));
			}
			if (ok) {
				ok = pending.consumeFence &&
					SUCCEEDED(g_device->OpenSharedHandle(
						pending.consumeFence, __uuidof(ID3D12Fence),
						reinterpret_cast<void**>(&g_consumeFence)));
			}
			CloseHandles(pending);
			if (!ok) {
				ReleaseRing();
				REX::ERROR("[WorldSurface] could not open the browser shared ring");
				return false;
			}
			g_slotCount = pending.slotCount;
			g_generation = pending.generation;
			REX::INFO("[WorldSurface] adopted dedicated {}x{} browser ring "
				"({} slots, generation {})",
				pending.width, pending.height, g_slotCount, g_generation);
			return true;
		}
	}

	void Configure(std::uint32_t a_targetWidth, std::uint32_t a_targetHeight)
	{
		g_targetWidth.store(a_targetWidth, std::memory_order_relaxed);
		g_targetHeight.store(a_targetHeight, std::memory_order_relaxed);
		g_enabled.store(a_targetWidth != 0 && a_targetHeight != 0,
			std::memory_order_release);
	}

	bool IsEnabled()
	{
		return g_enabled.load(std::memory_order_acquire);
	}

	bool TryInstall(ID3D12Device* a_device)
	{
		if (!IsEnabled() || !a_device) {
			return false;
		}
		if (g_installTried.exchange(true, std::memory_order_acq_rel)) {
			return g_installed.load(std::memory_order_acquire);
		}

		std::uintptr_t vtableAddress = 0;
		if (!Platform::SafeReadPointer(
				reinterpret_cast<std::uintptr_t>(a_device), vtableAddress)) {
			return false;
		}
		const auto slotAddress =
			vtableAddress + kCreateSrvSlot * sizeof(std::uintptr_t);
		std::uintptr_t current = 0;
		if (!Platform::SafeReadPointer(slotAddress, current) || current == 0) {
			return false;
		}
		auto** slot = reinterpret_cast<void**>(slotAddress);
		DWORD oldProtect = 0;
		if (!::VirtualProtect(slot, sizeof(void*), PAGE_READWRITE, &oldProtect)) {
			return false;
		}
		g_original.store(reinterpret_cast<CreateSrvFn>(current),
			std::memory_order_release);
		*slot = reinterpret_cast<void*>(&CreateSrvThunk);
		::VirtualProtect(slot, sizeof(void*), oldProtect, &oldProtect);

		{
			std::scoped_lock lock(g_mutex);
			a_device->AddRef();
			g_device = a_device;
		}
		g_installed.store(true, std::memory_order_release);
		REX::INFO("[WorldSurface] material binding armed for unique {}x{} "
			"placeholder textures",
			g_targetWidth.load(), g_targetHeight.load());
		return true;
	}

	void SetSharedRing(const SharedRingDesc& a_desc)
	{
		std::scoped_lock lock(g_mutex);
		if (g_pendingDirty) {
			CloseHandles(g_pending);
		}
		g_pending = a_desc;
		g_pendingDirty = true;
		AdoptPending();
	}

	void Submit(const FrameBufferView& a_frame)
	{
		if (a_frame.sharedSlot < 0 || a_frame.frameIndex == 0) {
			return;
		}
		std::scoped_lock lock(g_mutex);
		if (!AdoptPending()) {
			return;
		}
		// Consume pacing: the serial written into the descriptor LAST tick has
		// had a full engine frame submitted since, so the browser host may
		// overwrite its slot. Same CPU-side pacing as the overlay's seam draw —
		// ring depth covers residual GPU lag — but signaling the previously
		// displayed serial rather than the one being written right now keeps
		// the overwrite-vs-sample window empty in practice.
		if (g_pendingConsumeSerial != 0 && g_consumeFence) {
			g_consumeFence->Signal(g_pendingConsumeSerial);
			if (!g_loggedConsume) {
				g_loggedConsume = true;
				REX::INFO("[WorldSurface] consume signaling live (serial {})",
					g_pendingConsumeSerial);
			}
			g_pendingConsumeSerial = 0;
		}
		const auto slot = static_cast<std::uint32_t>(a_frame.sharedSlot);
		if (slot >= g_slotCount || !g_slots[slot] ||
			a_frame.frameIndex <= g_lastSerial) {
			return;
		}
		if (g_produceFence->GetCompletedValue() < a_frame.frameIndex) {
			// Bounded stall log: distinguishes "browser ring never completes a
			// frame" from "descriptor rewritten but visually wrong" in one log.
			++g_fenceStalls;
			if (g_fenceStalls == 60 || g_fenceStalls % 600 == 0) {
				REX::WARN("[WorldSurface] produce fence has not completed serial {} "
					"({} consecutive stalled submits)",
					a_frame.frameIndex, g_fenceStalls);
			}
			return;
		}
		g_lastSlot = slot;
		g_lastSerial = a_frame.frameIndex;
		// A frame can complete long before the cockpit material exists. Latch
			// the newest slot regardless (Refresh and the capture thunk both use
			// it), but only claim a descriptor write when one actually landed.
			const bool wrote = WriteReplacement(g_slots[slot]);
		// Signaled at the next Submit, once the engine frame sampling this
		// slot has been submitted (see the consume-pacing note above).
		g_pendingConsumeSerial = a_frame.frameIndex;
		if (wrote && !g_loggedWrite) {
			g_loggedWrite = true;
			REX::INFO("[WorldSurface] placeholder descriptor now samples browser "
				"ring slot {} (serial {})", slot, a_frame.frameIndex);
		}
		g_fenceStalls = 0;
	}

	void Refresh()
	{
		std::scoped_lock lock(g_mutex);
		if (g_targetSrv.ptr == 0 || g_slotCount == 0 || !g_slots[g_lastSlot]) {
			return;
		}
		// The browser publishes only on repaint, so a static page would leave the
		// descriptor written exactly once — and anything the engine does to that
		// descriptor afterwards (streaming residency, descriptor-heap rebuild)
		// would silently restore the placeholder with no second capture logged.
		// Rewriting every tick is one free-threaded CreateShaderResourceView call
		// and makes the binding self-healing. World surface only; the fullscreen
		// overlay never goes through this path.
		if (!WriteReplacement(g_slots[g_lastSlot])) {
			return;
		}
		if (++g_refreshWrites == g_nextRefreshLog) {
			g_nextRefreshLog *= 8;
			REX::INFO("[WorldSurface] descriptor refresh #{} -> slot {} "
				"(serial {}, srvCpu=0x{:X})",
				g_refreshWrites, g_lastSlot, g_lastSerial, g_targetSrv.ptr);
		}
	}

	void Shutdown()
	{
		std::scoped_lock lock(g_mutex);
		if (g_pendingDirty) {
			CloseHandles(g_pending);
			g_pendingDirty = false;
		}
		ReleaseRing();
		SafeRelease(g_device);
		g_targetSrv = {};
	}
}
