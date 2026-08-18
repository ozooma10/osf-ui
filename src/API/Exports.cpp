#include <intrin.h>  // _ReturnAddress — attributes a refusal to the calling plugin

#include "OSFUI_API.h"       // IOSFUIBridge, kBridgeAPIMajor
#include "API/BridgeApi.h"   // BridgeApi::Get()
#include "Platform/WindowsPlatform.h"

// The single undecorated C export a sibling SFSE plugin fetches once, after SFSE
// kPostLoad, via GetModuleHandleW("OSFUI.dll") + GetProcAddress. Consumers use
// OSFUI::API::RequestBridge in sdk/OSFUI_API.h.
//
// ABI 1.x is append-only: old binaries use their known vtable prefix and newer
// callers feature-gate appended tail methods by the returned minor version.
extern "C" __declspec(dllexport) OSFUI::API::IOSFUIBridge* OSFUI_RequestBridge(std::uint32_t a_abiVersion) noexcept
{
	const auto major = a_abiVersion >> 16;
	const auto minor = a_abiVersion & 0xFFFFu;
	if (major != OSFUI::API::kBridgeAPIMajor) {
		OSFUI::API::BridgeApi::Get().NoteUnsupportedApiCaller(
			OSFUI::Platform::ModuleNameForAddress(_ReturnAddress()), major, minor);
		return nullptr;
	}
	// A caller minor above the OSF UI runtime's is legal: it must gate tail vmethods on
	// GetInterfaceVersion, as the Client wrapper does.
	REX::INFO("BridgeApi: bridge vended (caller ABI {}.{}, runtime ABI {}.{})",
		major, minor, OSFUI::API::kBridgeAPIMajor, OSFUI::API::kBridgeAPIMinor);
	return &OSFUI::API::BridgeApi::Get();
}
