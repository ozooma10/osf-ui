#include <intrin.h>  // _ReturnAddress — attributes a refusal to the calling plugin

#include "OSFUI_API.h"
#include "API/BridgeApi.h"
#include "Compat/V1/LegacyBridge.h"
#include "Platform/WindowsPlatform.h"

namespace
{
	constexpr std::uint32_t Major(std::uint32_t a_version) noexcept { return a_version >> 16; }
	constexpr std::uint32_t Minor(std::uint32_t a_version) noexcept { return a_version & 0xFFFFu; }

	void* RequestService(const char* a_name, std::uint32_t a_requested, std::uint32_t a_supported,
		void* a_service, std::uint32_t* a_outVersion) noexcept
	{
		if (a_outVersion) *a_outVersion = 0;
		if (Major(a_requested) != Major(a_supported) || Minor(a_requested) > Minor(a_supported)) {
			REX::WARN("BridgeApi: {} service refused version {}.{}; runtime is {}.{}",
				a_name, Major(a_requested), Minor(a_requested), Major(a_supported), Minor(a_supported));
			return nullptr;
		}
		if (a_outVersion) *a_outVersion = a_supported;
		REX::INFO("BridgeApi: {} service vended at {}.{}", a_name, Major(a_supported), Minor(a_supported));
		return a_service;
	}
}

extern "C" __declspec(dllexport) OSFUI::API::IOSFUIBridge* OSFUI_RequestBridge(
	std::uint32_t a_abiVersion) noexcept
{
	const auto major = Major(a_abiVersion);
	const auto minor = Minor(a_abiVersion);
	if (major != OSFUI::API::kBridgeAPIMajor) {
		OSFUI::API::BridgeApi::Get().NoteUnsupportedApiCaller(
			OSFUI::Platform::ModuleNameForAddress(_ReturnAddress()), major, minor);
		return nullptr;
	}
	REX::INFO("BridgeApi: legacy bridge vended (caller ABI {}.{}, runtime ABI {}.{})",
		major, minor, OSFUI::API::kBridgeAPIMajor, OSFUI::API::kBridgeAPIMinor);
	return static_cast<OSFUI::API::IOSFUIBridge*>(&OSFUI::API::Legacy::Bridge::Get());
}

extern "C" __declspec(dllexport) void* OSFUI_RequestSettings(
	std::uint32_t a_version, std::uint32_t* a_outVersion) noexcept
{
	return RequestService("settings", a_version, OSFUI::API::Settings::kVersion,
		static_cast<OSFUI::API::Settings::ISettings*>(&OSFUI::API::BridgeApi::Get()), a_outVersion);
}

extern "C" __declspec(dllexport) void* OSFUI_RequestViews(
	std::uint32_t a_version, std::uint32_t* a_outVersion) noexcept
{
	return RequestService("views", a_version, OSFUI::API::Views::kVersion,
		static_cast<OSFUI::API::Views::IViews*>(&OSFUI::API::BridgeApi::Get()), a_outVersion);
}

extern "C" __declspec(dllexport) void* OSFUI_RequestDiagnostics(
	std::uint32_t a_version, std::uint32_t* a_outVersion) noexcept
{
	return RequestService("diagnostics", a_version, OSFUI::API::Diagnostics::kVersion,
		static_cast<OSFUI::API::Diagnostics::IDiagnostics*>(&OSFUI::API::BridgeApi::Get()), a_outVersion);
}
