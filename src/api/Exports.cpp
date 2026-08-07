#include <intrin.h>  // _ReturnAddress — attributes a refusal to the calling plugin

#include "OSFUI_API.h"       // IOSFUIBridge, kBridgeAPIMajor
#include "api/BridgeApi.h"   // BridgeApi::Get()
#include "compat/v1/NativeBridge.h"
#include "platform/WindowsPlatform.h"

// The single undecorated C export a sibling SFSE plugin fetches once, after SFSE
// kPostLoad, via GetModuleHandleW("OSFUI.dll") + GetProcAddress (see
// OSFUI::API::RequestBridge in sdk/OSFUI_API.h and docs/native-plugin-api.md).
//
// ABI 1.x is temporarily adapted and reported through a bounded local Health
// issue naming the outdated DLL. Future ABI 2.x minors remain additive.
extern "C" __declspec(dllexport) OSFUI::API::IOSFUIBridge* OSFUI_RequestBridge(std::uint32_t a_abiVersion) noexcept
{
	const auto major = a_abiVersion >> 16;
	const auto minor = a_abiVersion & 0xFFFFu;
	if (OSFUI::Compat::V1::SupportsRequestedAbi(a_abiVersion)) {
		OSFUI::API::BridgeApi::Get().NoteLegacyApiCaller(
			OSFUI::Platform::ModuleNameForAddress(_ReturnAddress()), major, minor, true);
		return reinterpret_cast<OSFUI::API::IOSFUIBridge*>(
			&OSFUI::Compat::V1::NativeBridge::Get());
	}
	if (major != OSFUI::API::kBridgeAPIMajor) {
		// The caller reached us through GetProcAddress from its own code, so the
		// return address is inside its module: that names the DLL the player has
		// to update, instead of an issue that says only "some mod". The health registry
		// pump owns the deduplicated log entry as well as the persistent issue.
		OSFUI::API::BridgeApi::Get().NoteLegacyApiCaller(
			OSFUI::Platform::ModuleNameForAddress(_ReturnAddress()), major, minor, false);
		return nullptr;
	}
	// A caller minor above the OSF UI runtime's is legal: it must gate tail vmethods on
	// GetInterfaceVersion, as the Client wrapper does.
	REX::INFO("BridgeApi: bridge vended (caller ABI {}.{}, runtime ABI {}.{})",
		major, minor, OSFUI::API::kBridgeAPIMajor, OSFUI::API::kBridgeAPIMinor);
	return &OSFUI::API::BridgeApi::Get();
}
