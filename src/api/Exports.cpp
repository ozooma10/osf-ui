#include <intrin.h>  // _ReturnAddress — attributes a refusal to the calling plugin

#include "OSFUI_API.h"       // IOSFUIBridge, kBridgeAPIMajor
#include "api/BridgeApi.h"   // BridgeApi::Get()
#include "platform/WindowsPlatform.h"

// The single undecorated C export a sibling SFSE plugin fetches once, after SFSE
// kPostLoad, via GetModuleHandleW("OSFUI.dll") + GetProcAddress (see
// OSFUI::API::RequestBridge in sdk/OSFUI_API.h and docs/native-plugin-api.md).
//
// Major must match: ABI 1.x callers get nullptr and a bounded local Health
// diagnostic naming the outdated DLL. Future ABI 2.x minors remain additive.
extern "C" __declspec(dllexport) OSFUI::API::IOSFUIBridge* OSFUI_RequestBridge(std::uint32_t a_abiVersion) noexcept
{
	const auto major = a_abiVersion >> 16;
	const auto minor = a_abiVersion & 0xFFFFu;
	if (major != OSFUI::API::kBridgeAPIMajor) {
		REX::WARN("BridgeApi: OSFUI_RequestBridge refused — caller built against ABI {}.{}, host is {}.{} (MAJOR mismatch)",
			major, minor, OSFUI::API::kBridgeAPIMajor, OSFUI::API::kBridgeAPIMinor);
		// The caller reached us through GetProcAddress from its own code, so the
		// return address is inside its module: that names the DLL the player has
		// to update, instead of a card that says only "some mod".
		OSFUI::API::BridgeApi::Get().NoteLegacyApiCaller(
			OSFUI::Platform::ModuleNameForAddress(_ReturnAddress()), major, minor);
		return nullptr;
	}
	// A caller minor above the host's is legal: it must gate tail vmethods on
	// GetInterfaceVersion, as the Client wrapper does.
	REX::INFO("BridgeApi: bridge vended (caller ABI {}.{}, host {}.{})",
		major, minor, OSFUI::API::kBridgeAPIMajor, OSFUI::API::kBridgeAPIMinor);
	return &OSFUI::API::BridgeApi::Get();
}
