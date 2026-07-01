#include "OSFUI_API.h"       // IOSFUIBridge, kBridgeAPIMajor
#include "api/BridgeApi.h"   // BridgeApi::Get()

// The single undecorated C export a sibling SFSE plugin fetches once, after SFSE
// kPostLoad, via GetModuleHandleW("OSFUI.dll") + GetProcAddress (see
// OSFUI::API::RequestBridge in sdk/OSFUI_API.h and docs/native-plugin-api.md).
//
// MAJOR must match: a caller built against a different MAJOR gets nullptr and
// degrades. MINOR differences are backward-compatible (the vtable only grows at
// the end), so they are accepted here.
extern "C" __declspec(dllexport) OSFUI::API::IOSFUIBridge* OSFUI_RequestBridge(std::uint32_t a_abiVersion) noexcept
{
	if ((a_abiVersion >> 16) != OSFUI::API::kBridgeAPIMajor) {
		return nullptr;
	}
	return &OSFUI::API::BridgeApi::Get();
}
