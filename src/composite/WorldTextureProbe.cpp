#include "composite/WorldTextureProbe.h"

#include "platform/WindowsPlatform.h"

#include "composite/D3D12Prologue.h"

#include <intrin.h>

#include <atomic>
#include <mutex>
#include <cstdint>
#include <cstring>
#include <string>

namespace OSFUI::WorldTextureProbe
{
	namespace
	{
		// IUnknown (0..2), ID3D12Object (3..6), then ID3D12Device. This is
		// ID3D12Device::CreateShaderResourceView in the published interface ABI.
		constexpr std::size_t kCreateSrvSlot = 18;

		// Temporary loose diagnostic override for the proven vanilla asset:
		// textures/ships/interior/cockpitscreens/
		//     shipscreen_avionics01_color.dds
		// Its unusual dimensions isolate one resource without relying on names.
		constexpr UINT64 kTargetWidth = 1000;
		constexpr UINT   kTargetHeight = 1000;

		using CreateSrvFn = void(STDMETHODCALLTYPE*)(
			ID3D12Device*,
			ID3D12Resource*,
			const D3D12_SHADER_RESOURCE_VIEW_DESC*,
			D3D12_CPU_DESCRIPTOR_HANDLE);

		std::atomic<bool>        g_enabled{ false };
		std::atomic<bool>        g_installTried{ false };
		std::atomic<bool>        g_installed{ false };
		std::atomic<CreateSrvFn> g_original{ nullptr };
		std::atomic<std::uint64_t> g_matches{ 0 };
		std::mutex       g_replacementMutex;
		ID3D12Resource* g_replacement{ nullptr };

		[[nodiscard]] ID3D12Resource* AcquireReplacement()
		{
			std::scoped_lock lock(g_replacementMutex);
			auto* replacement = g_replacement;
			if (replacement) {
				replacement->AddRef();
			}
			return replacement;
		}


		[[nodiscard]] bool IsTargetSignature(
			const D3D12_RESOURCE_DESC& a_resource,
			const D3D12_SHADER_RESOURCE_VIEW_DESC*)
		{
			return a_resource.Dimension == D3D12_RESOURCE_DIMENSION_TEXTURE2D &&
				a_resource.Width == kTargetWidth &&
				a_resource.Height == kTargetHeight;
		}

		[[nodiscard]] std::string ResourceDebugName(ID3D12Resource* a_resource)
		{
			wchar_t wide[512]{};
			UINT wideBytes = sizeof(wide) - sizeof(wchar_t);
			if (SUCCEEDED(a_resource->GetPrivateData(
					WKPDID_D3DDebugObjectNameW, &wideBytes, wide)) &&
				wideBytes >= sizeof(wchar_t)) {
				const auto wideChars = static_cast<int>(wideBytes / sizeof(wchar_t));
				char utf8[1536]{};
				const auto written = ::WideCharToMultiByte(
					CP_UTF8, 0, wide, wideChars, utf8,
					static_cast<int>(sizeof(utf8) - 1), nullptr, nullptr);
				if (written > 0) {
					return std::string(utf8, static_cast<std::size_t>(written));
				}
			}

			char narrow[512]{};
			UINT narrowBytes = sizeof(narrow) - 1;
			if (SUCCEEDED(a_resource->GetPrivateData(
					WKPDID_D3DDebugObjectName, &narrowBytes, narrow)) &&
				narrowBytes > 0) {
				return std::string(narrow, narrowBytes);
			}
			return "<unnamed>";
		}

		void STDMETHODCALLTYPE CreateSrvThunk(
			ID3D12Device* a_device,
			ID3D12Resource* a_resource,
			const D3D12_SHADER_RESOURCE_VIEW_DESC* a_desc,
			D3D12_CPU_DESCRIPTOR_HANDLE a_destination)
		{
			bool target = false;
			if (a_resource) {
				const auto resourceDesc = a_resource->GetDesc();
				target = IsTargetSignature(resourceDesc, a_desc);
				if (target) {
					const auto match = g_matches.fetch_add(1, std::memory_order_relaxed) + 1;
					const auto caller = reinterpret_cast<std::uintptr_t>(_ReturnAddress());
					const auto imageBase = reinterpret_cast<std::uintptr_t>(::GetModuleHandleW(nullptr));
					const auto callerRva = caller >= imageBase ? caller - imageBase : 0;
					const auto viewFormat = a_desc ?
						static_cast<std::uint32_t>(a_desc->Format) :
						static_cast<std::uint32_t>(DXGI_FORMAT_UNKNOWN);
					const auto viewDimension = a_desc ?
						static_cast<std::uint32_t>(a_desc->ViewDimension) : 0;

					// Creation is naturally bounded (normally once per loaded
					// texture), but cap logging in case a mod churns resources.
					if (match <= 128) {
						const auto resourceName = ResourceDebugName(a_resource);
						REX::INFO(
							"[WorldTextureProbe] candidate={} resource=0x{:X} "
							"size={}x{} resourceFormat={} viewFormat={} viewDimension={} "
							"mips={} srvCpu=0x{:X} name='{}' caller=0x{:X} callerRva=0x{:X}",
							match,
							reinterpret_cast<std::uintptr_t>(a_resource),
							resourceDesc.Width, resourceDesc.Height,
							static_cast<std::uint32_t>(resourceDesc.Format),
							viewFormat, viewDimension, resourceDesc.MipLevels,
							a_destination.ptr, resourceName, caller, callerRva);
					}
				}
			}

			const auto original = g_original.load(std::memory_order_acquire);
			if (original && target) {
				auto* replacement = AcquireReplacement();
				if (replacement) {
					D3D12_SHADER_RESOURCE_VIEW_DESC replacementSrv{};
					replacementSrv.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
					replacementSrv.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
					replacementSrv.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
					replacementSrv.Texture2D.MipLevels = 1;
					original(a_device, replacement, &replacementSrv,
						a_destination);
					REX::INFO("[WorldTextureProbe] substituted WebView ring slot 0 into srvCpu=0x{:X}",
						a_destination.ptr);
					replacement->Release();
					return;
				}
			}

			if (original) {
				original(a_device, a_resource, a_desc, a_destination);
			}
		}
	}

	void Enable()
	{
		g_enabled.store(true, std::memory_order_release);
	}

	bool IsEnabled()
	{
		return g_enabled.load(std::memory_order_acquire);
	}

	void SetReplacementTexture(ID3D12Resource* a_resource)
	{
		if (!IsEnabled()) {
			return;
		}

		ID3D12Resource* previous = nullptr;
		{
			std::scoped_lock lock(g_replacementMutex);
			if (g_replacement == a_resource) {
				return;
			}
			if (a_resource) {
				a_resource->AddRef();
			}
			previous = g_replacement;
			g_replacement = a_resource;
		}
		if (previous) {
			previous->Release();
		}
		if (a_resource) {
			REX::INFO("[WorldTextureProbe] WebView replacement texture ready at 0x{:X}",
				reinterpret_cast<std::uintptr_t>(a_resource));
		} else {
			REX::DEBUG("[WorldTextureProbe] WebView replacement texture cleared");
		}
	}
	bool TryInstall(ID3D12Device* a_device)
	{
		if (!g_enabled.load(std::memory_order_acquire) || !a_device) {
			return false;
		}
		if (g_installTried.exchange(true, std::memory_order_acq_rel)) {
			return g_installed.load(std::memory_order_acquire);
		}

		std::uintptr_t vtableAddress = 0;
		if (!Platform::SafeReadPointer(
				reinterpret_cast<std::uintptr_t>(a_device), vtableAddress)) {
			REX::WARN("[WorldTextureProbe] device vtable unreadable; probe not installed");
			return false;
		}

		const auto slotAddress =
			vtableAddress + kCreateSrvSlot * sizeof(std::uintptr_t);
		std::uintptr_t current = 0;
		if (!Platform::SafeReadPointer(slotAddress, current) || current == 0) {
			REX::WARN("[WorldTextureProbe] CreateShaderResourceView slot unreadable; probe not installed");
			return false;
		}

		auto** slot = reinterpret_cast<void**>(slotAddress);
		DWORD oldProtect = 0;
		if (!::VirtualProtect(slot, sizeof(void*), PAGE_READWRITE, &oldProtect)) {
			REX::WARN("[WorldTextureProbe] VirtualProtect failed; probe not installed");
			return false;
		}

		g_original.store(reinterpret_cast<CreateSrvFn>(current), std::memory_order_release);
		*slot = reinterpret_cast<void*>(&CreateSrvThunk);
		::VirtualProtect(slot, sizeof(void*), oldProtect, &oldProtect);

		std::uintptr_t installed = 0;
		if (!Platform::SafeReadPointer(slotAddress, installed) ||
			installed != reinterpret_cast<std::uintptr_t>(&CreateSrvThunk)) {
			REX::WARN("[WorldTextureProbe] vtable read-back failed; probe state is unknown");
			return false;
		}

		g_installed.store(true, std::memory_order_release);
		REX::INFO(
			"[WorldTextureProbe] armed on ID3D12Device::CreateShaderResourceView "
			"(slot {}, original 0x{:X}); target signature is 1000x1000 diagnostic DDS",
			kCreateSrvSlot, current);
		return true;
	}
}
