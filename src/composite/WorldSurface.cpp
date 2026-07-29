#include "composite/WorldSurface.h"

#include "composite/D3D12Prologue.h"
#include "platform/WindowsPlatform.h"

#include <array>
#include <atomic>
#include <mutex>

namespace OSFUI::WorldSurface
{
	namespace
	{
		constexpr std::size_t kCreateSrvSlot = 18;
		// WebView2 publishes BGRA bytes in sRGB display space. Vanilla display
		// textures are also authored as sRGB resources (for example,
		// DisplayScreenWhite1 is BC1_UNORM_SRGB), so sampling the shared ring
		// through a linear UNORM view lifts midtones and washes out the page.
		constexpr DXGI_FORMAT kBrowserSrvFormat =
			DXGI_FORMAT_B8G8R8A8_UNORM_SRGB;

		using CreateSrvFn = void(STDMETHODCALLTYPE*)(
			ID3D12Device*, ID3D12Resource*,
			const D3D12_SHADER_RESOURCE_VIEW_DESC*,
			D3D12_CPU_DESCRIPTOR_HANDLE);

		// Per-surface state. The signature fields are immutable after
		// Configure and read lock-free by the SRV thunk (published by the
		// release store of g_surfaceCount); everything else is under g_mutex.
		struct Surface
		{
			std::uint32_t placeholderWidth{ 0 };
			std::uint32_t placeholderHeight{ 0 };
			std::string   label;

			// The authored material may bind the placeholder more than once
			// (Albedo + Emissive), so one surface owns a small SET of captured
			// descriptors, deduped by CPU handle.
			static constexpr std::uint32_t kMaxCapturedSrvs = 4;
			std::array<D3D12_CPU_DESCRIPTOR_HANDLE, kMaxCapturedSrvs> capturedSrvs{};
			std::uint32_t capturedCount{ 0 };
			bool          capturedOverflowWarned{ false };

			ID3D12Resource* slots[SharedRingDesc::kMaxSlots]{};
			std::uint32_t   slotCount{ 0 };
			ID3D12Fence*    produceFence{ nullptr };
			ID3D12Fence*    consumeFence{ nullptr };
			SharedRingDesc  pending{};
			bool            pendingDirty{ false };
			std::uint64_t   generation{ 0 };
			std::uint64_t   lastSerial{ 0 };
			std::uint32_t   lastSlot{ 0 };
			std::uint64_t   pendingConsumeSerial{ 0 };
			bool            loggedWrite{ false };
			bool            loggedConsume{ false };
			std::uint32_t   fenceStalls{ 0 };
			std::uint32_t   captures{ 0 };
			std::uint64_t   refreshWrites{ 0 };
			std::uint64_t   nextRefreshLog{ 1 };
		};

		std::atomic_bool g_enabled{ false };
		std::atomic_bool g_installTried{ false };
		std::atomic_bool g_installed{ false };
		std::atomic<CreateSrvFn> g_original{ nullptr };
		std::array<Surface, kMaxSurfaces> g_surfaces;
		std::atomic<std::uint32_t> g_surfaceCount{ 0 };

		// One lock for all surfaces: N <= 4, the SRV thunk only takes it on a
		// signature hit, and the only expensive section under it —
		// OpenSharedHandle in AdoptPending — runs once per ring generation,
		// not per frame.
		std::mutex g_mutex;
		ID3D12Device* g_device{ nullptr };

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

		// Ring-side reset only: captured descriptors identify the MATERIAL and
		// stay valid across ring generations, so they survive here.
		void ReleaseRing(Surface& a_surface)
		{
			for (auto*& slot : a_surface.slots) {
				SafeRelease(slot);
			}
			SafeRelease(a_surface.produceFence);
			SafeRelease(a_surface.consumeFence);
			a_surface.slotCount = 0;
			a_surface.lastSerial = 0;
			a_surface.lastSlot = 0;
			a_surface.pendingConsumeSerial = 0;
			a_surface.fenceStalls = 0;
			a_surface.loggedWrite = false;
			a_surface.loggedConsume = false;
		}

		// Dimensions alone are NOT a safe signature. The engine allocates its
		// own render targets, and matching one of those rewrites a descriptor
		// the frame depends on — which breaks rendering globally, not just the
		// surface. A verified OSF UI placeholder reaches CreateSRV as a plain
		// typeless BGRA texture: no render-target/depth/UAV capability, single
		// slice, single mip. Requiring that exact shape and resource format
		// excludes engine-owned targets and unrelated same-sized textures; the
		// per-surface size then disambiguates our canonical placeholders.
		[[nodiscard]] Surface* FindTarget(ID3D12Resource* a_resource)
		{
			if (!a_resource) {
				return nullptr;
			}
			const auto desc = a_resource->GetDesc();
			if (desc.Dimension != D3D12_RESOURCE_DIMENSION_TEXTURE2D ||
				desc.Format != DXGI_FORMAT_B8G8R8A8_TYPELESS ||
				desc.Flags != D3D12_RESOURCE_FLAG_NONE ||
				desc.DepthOrArraySize != 1 ||
				desc.MipLevels != 1 ||
				desc.SampleDesc.Count != 1) {
				return nullptr;
			}
			const auto count = g_surfaceCount.load(std::memory_order_acquire);
			for (std::uint32_t i = 0; i < count; ++i) {
				auto& surface = g_surfaces[i];
				if (desc.Width == surface.placeholderWidth &&
					desc.Height == surface.placeholderHeight) {
					return &surface;
				}
			}
			return nullptr;
		}

		// Returns false when no material descriptor has been captured yet, so
		// callers never latch "displayed" state for a write that did not land.
		bool WriteReplacement(Surface& a_surface, ID3D12Resource* a_resource)
		{
			const auto original = g_original.load(std::memory_order_acquire);
			if (!original || !g_device || !a_resource || a_surface.capturedCount == 0) {
				return false;
			}
			D3D12_SHADER_RESOURCE_VIEW_DESC srv{};
			srv.Format = kBrowserSrvFormat;
			srv.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
			srv.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
			srv.Texture2D.MipLevels = 1;
			for (std::uint32_t i = 0; i < a_surface.capturedCount; ++i) {
				original(g_device, a_resource, &srv, a_surface.capturedSrvs[i]);
			}
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
			auto* target = FindTarget(a_resource);
			if (!target) {
				original(a_device, a_resource, a_desc, a_destination);
				return;
			}
			auto& surface = *target;

			std::scoped_lock lock(g_mutex);
			// Dedupe by CPU handle: a repeat of a known handle is the engine
			// re-creating that descriptor (streaming/heap rebuild) — logged
			// below, because it is the prime suspect whenever a landed write
			// stops being visible. A new handle is another binding of the same
			// material (Albedo + Emissive) or a second descriptor heap.
			bool known = false;
			for (std::uint32_t i = 0; i < surface.capturedCount; ++i) {
				if (surface.capturedSrvs[i].ptr == a_destination.ptr) {
					known = true;
					break;
				}
			}
			if (!known) {
				if (surface.capturedCount < Surface::kMaxCapturedSrvs) {
					surface.capturedSrvs[surface.capturedCount++] = a_destination;
				} else {
					surface.capturedSrvs[Surface::kMaxCapturedSrvs - 1] = a_destination;
					if (!surface.capturedOverflowWarned) {
						surface.capturedOverflowWarned = true;
						REX::WARN("[WorldSurface] surface '{}' exceeded {} captured "
							"descriptors; keeping the newest — expect flicker if the "
							"evicted binding is still sampled",
							surface.label, Surface::kMaxCapturedSrvs);
					}
				}
			}
			const bool replaced = surface.slotCount != 0 &&
				surface.slots[surface.lastSlot] != nullptr;
			if (replaced) {
				D3D12_SHADER_RESOURCE_VIEW_DESC srv{};
				srv.Format = kBrowserSrvFormat;
				srv.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
				srv.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
				srv.Texture2D.MipLevels = 1;
				original(a_device, surface.slots[surface.lastSlot], &srv, a_destination);
			} else {
				original(a_device, a_resource, a_desc, a_destination);
			}
			const auto resource = a_resource->GetDesc();
			REX::INFO("[WorldSurface] surface '{}' captured placeholder {}x{} at "
				"srvCpu=0x{:X} ({} descriptors, known={}, resFormat {}, mips {}, "
				"viewFormat {}, replaced={})",
				surface.label, surface.placeholderWidth, surface.placeholderHeight,
				a_destination.ptr, surface.capturedCount, known,
				static_cast<int>(resource.Format), resource.MipLevels,
				a_desc ? static_cast<int>(a_desc->Format) : -1, replaced);
			// One material owns each placeholder, so captures should be rare.
			// A stream of them means that signature is colliding with something
			// the engine allocates — the failure mode that breaks the whole
			// frame.
			if (++surface.captures == 8) {
				REX::WARN("[WorldSurface] {} captures for surface '{}' — the {}x{} "
					"signature is probably colliding with engine-owned textures; "
					"change the placeholder to an implausible size",
					surface.captures, surface.label,
					surface.placeholderWidth, surface.placeholderHeight);
			}
		}

		bool AdoptPending(Surface& a_surface)
		{
			if (!a_surface.pendingDirty || !g_device) {
				return a_surface.slotCount != 0;
			}
			auto pending = a_surface.pending;
			a_surface.pending = {};
			a_surface.pendingDirty = false;
			ReleaseRing(a_surface);

			bool ok = pending.slotCount > 0 &&
				pending.slotCount <= SharedRingDesc::kMaxSlots;
			for (std::uint32_t i = 0; ok && i < pending.slotCount; ++i) {
				ok = pending.slotHandles[i] &&
					SUCCEEDED(g_device->OpenSharedHandle(
						pending.slotHandles[i], __uuidof(ID3D12Resource),
						reinterpret_cast<void**>(&a_surface.slots[i])));
			}
			if (ok) {
				ok = pending.produceFence &&
					SUCCEEDED(g_device->OpenSharedHandle(
						pending.produceFence, __uuidof(ID3D12Fence),
						reinterpret_cast<void**>(&a_surface.produceFence)));
			}
			if (ok) {
				ok = pending.consumeFence &&
					SUCCEEDED(g_device->OpenSharedHandle(
						pending.consumeFence, __uuidof(ID3D12Fence),
						reinterpret_cast<void**>(&a_surface.consumeFence)));
			}
			CloseHandles(pending);
			if (!ok) {
				ReleaseRing(a_surface);
				REX::ERROR("[WorldSurface] surface '{}' could not open its browser "
					"shared ring", a_surface.label);
				return false;
			}
			a_surface.slotCount = pending.slotCount;
			a_surface.generation = pending.generation;
			REX::INFO("[WorldSurface] surface '{}' adopted dedicated {}x{} browser "
				"ring ({} slots, generation {})",
				a_surface.label, pending.width, pending.height,
				a_surface.slotCount, a_surface.generation);
			return true;
		}
	}

	std::uint32_t Configure(std::span<const SurfaceDesc> a_surfaces)
	{
		std::uint32_t accepted = 0;
		for (const auto& desc : a_surfaces) {
			if (accepted >= kMaxSurfaces) {
				REX::WARN("[WorldSurface] surface '{}' dropped: {}-surface cap",
					desc.label, kMaxSurfaces);
				continue;
			}
			if (desc.placeholderWidth == 0 || desc.placeholderHeight == 0) {
				REX::WARN("[WorldSurface] surface '{}' dropped: zero placeholder size",
					desc.label);
				continue;
			}
			auto& surface = g_surfaces[accepted];
			surface.placeholderWidth = desc.placeholderWidth;
			surface.placeholderHeight = desc.placeholderHeight;
			surface.label = desc.label;
			++accepted;
		}
		// Release-published so the SRV thunk's acquire load sees fully written
		// signature fields.
		g_surfaceCount.store(accepted, std::memory_order_release);
		g_enabled.store(accepted != 0, std::memory_order_release);
		return accepted;
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
		std::string sizes;
		const auto count = g_surfaceCount.load(std::memory_order_acquire);
		for (std::uint32_t i = 0; i < count; ++i) {
			sizes += std::format("{}{}x{} ('{}')", sizes.empty() ? "" : ", ",
				g_surfaces[i].placeholderWidth, g_surfaces[i].placeholderHeight,
				g_surfaces[i].label);
		}
		REX::INFO("[WorldSurface] material binding armed for {} unique placeholder "
			"signature(s): {}", count, sizes);
		return true;
	}

	void SetSharedRing(std::uint32_t a_surface, const SharedRingDesc& a_desc)
	{
		std::scoped_lock lock(g_mutex);
		if (a_surface >= g_surfaceCount.load(std::memory_order_acquire)) {
			// This call owns the duplicated handles even when the index is
			// nonsense — closing them here is what keeps a wiring bug a log
			// line instead of a handle leak.
			REX::ERROR("[WorldSurface] SetSharedRing for unknown surface {}", a_surface);
			auto desc = a_desc;
			CloseHandles(desc);
			return;
		}
		auto& surface = g_surfaces[a_surface];
		if (surface.pendingDirty) {
			CloseHandles(surface.pending);
		}
		surface.pending = a_desc;
		surface.pendingDirty = true;
		AdoptPending(surface);
	}

	void Submit(std::uint32_t a_surface, const FrameBufferView& a_frame)
	{
		if (a_frame.sharedSlot < 0 || a_frame.frameIndex == 0) {
			return;
		}
		std::scoped_lock lock(g_mutex);
		if (a_surface >= g_surfaceCount.load(std::memory_order_acquire)) {
			return;
		}
		auto& surface = g_surfaces[a_surface];
		if (!AdoptPending(surface)) {
			return;
		}
		// Consume pacing: the serial written into the descriptor LAST tick has
		// had a full engine frame submitted since, so the browser host may
		// overwrite its slot. Same CPU-side pacing as the overlay's seam draw —
		// ring depth covers residual GPU lag — but signaling the previously
		// displayed serial rather than the one being written right now keeps
		// the overwrite-vs-sample window empty in practice.
		if (surface.pendingConsumeSerial != 0 && surface.consumeFence) {
			surface.consumeFence->Signal(surface.pendingConsumeSerial);
			if (!surface.loggedConsume) {
				surface.loggedConsume = true;
				REX::INFO("[WorldSurface] surface '{}' consume signaling live (serial {})",
					surface.label, surface.pendingConsumeSerial);
			}
			surface.pendingConsumeSerial = 0;
		}
		const auto slot = static_cast<std::uint32_t>(a_frame.sharedSlot);
		if (slot >= surface.slotCount || !surface.slots[slot] ||
			a_frame.frameIndex <= surface.lastSerial) {
			return;
		}
		if (surface.produceFence->GetCompletedValue() < a_frame.frameIndex) {
			// Bounded stall log: distinguishes "browser ring never completes a
			// frame" from "descriptor rewritten but visually wrong" in one log.
			++surface.fenceStalls;
			if (surface.fenceStalls == 60 || surface.fenceStalls % 600 == 0) {
				REX::WARN("[WorldSurface] surface '{}' produce fence has not completed "
					"serial {} ({} consecutive stalled submits)",
					surface.label, a_frame.frameIndex, surface.fenceStalls);
			}
			return;
		}
		surface.lastSlot = slot;
		surface.lastSerial = a_frame.frameIndex;
		// A frame can complete long before the world material exists. Latch
		// the newest slot regardless (Refresh and the capture thunk both use
		// it), but only claim a descriptor write when one actually landed.
		const bool wrote = WriteReplacement(surface, surface.slots[slot]);
		// Signaled at the next Submit, once the engine frame sampling this
		// slot has been submitted (see the consume-pacing note above).
		surface.pendingConsumeSerial = a_frame.frameIndex;
		if (wrote && !surface.loggedWrite) {
			surface.loggedWrite = true;
			REX::INFO("[WorldSurface] surface '{}' placeholder descriptor now samples "
				"browser ring slot {} (serial {})",
				surface.label, slot, a_frame.frameIndex);
		}
		surface.fenceStalls = 0;
	}

	void Refresh()
	{
		std::scoped_lock lock(g_mutex);
		const auto count = g_surfaceCount.load(std::memory_order_acquire);
		for (std::uint32_t i = 0; i < count; ++i) {
			auto& surface = g_surfaces[i];
			if (surface.capturedCount == 0 || surface.slotCount == 0 ||
				!surface.slots[surface.lastSlot]) {
				continue;
			}
			// The browser publishes only on repaint, so a static page would
			// leave each descriptor written exactly once — and anything the
			// engine does to that descriptor afterwards (streaming residency,
			// descriptor-heap rebuild) would silently restore the placeholder
			// with no second capture logged. Rewriting every tick is a few
			// free-threaded CreateShaderResourceView calls and makes the
			// binding self-healing. World surfaces only; the fullscreen
			// overlay never goes through this path.
			if (!WriteReplacement(surface, surface.slots[surface.lastSlot])) {
				continue;
			}
			if (++surface.refreshWrites == surface.nextRefreshLog) {
				surface.nextRefreshLog *= 8;
				REX::INFO("[WorldSurface] surface '{}' descriptor refresh #{} -> "
					"slot {} (serial {}, {} descriptors)",
					surface.label, surface.refreshWrites, surface.lastSlot,
					surface.lastSerial, surface.capturedCount);
			}
		}
	}

	void Shutdown()
	{
		std::scoped_lock lock(g_mutex);
		const auto count = g_surfaceCount.load(std::memory_order_acquire);
		for (std::uint32_t i = 0; i < count; ++i) {
			auto& surface = g_surfaces[i];
			if (surface.pendingDirty) {
				CloseHandles(surface.pending);
				surface.pendingDirty = false;
			}
			ReleaseRing(surface);
			surface.capturedSrvs = {};
			surface.capturedCount = 0;
		}
		SafeRelease(g_device);
	}
}
