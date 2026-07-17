#include "OSFUI_API.h"       // IOSFUIBridge, kBridgeAPIMajor
#include "api/BridgeApi.h"   // BridgeApi::Get()

// The single undecorated C export a sibling SFSE plugin fetches once, after SFSE
// kPostLoad, via GetModuleHandleW("OSFUI.dll") + GetProcAddress (see
// OSFUI::API::RequestBridge in sdk/OSFUI_API.h and docs/native-plugin-api.md).
//
// MAJOR must match: a caller built against a different MAJOR gets nullptr and
// degrades. MINOR differences are backward-compatible (the vtable only grows at
// the end), so they are accepted here. Multi-major policy (documented in the
// header): if a 2.0 ever exists, this export becomes a per-MAJOR dispatcher
// that keeps vending the v1 interface to v1 callers.
extern "C" __declspec(dllexport) OSFUI::API::IOSFUIBridge* OSFUI_RequestBridge(std::uint32_t a_abiVersion) noexcept
{
	const auto major = a_abiVersion >> 16;
	const auto minor = a_abiVersion & 0xFFFFu;
	if (major != OSFUI::API::kBridgeAPIMajor) {
		REX::WARN("BridgeApi: OSFUI_RequestBridge refused — caller built against ABI {}.{}, host is {}.{} (MAJOR mismatch)",
			major, minor, OSFUI::API::kBridgeAPIMajor, OSFUI::API::kBridgeAPIMinor);
		return nullptr;
	}
	// Cheap support telemetry: which header vintage each consumer was built
	// against. A caller MINOR above the host's is legal (it must gate tail
	// vmethods on GetInterfaceVersion — the Client wrapper does).
	REX::INFO("BridgeApi: bridge vended (caller ABI {}.{}, host {}.{})",
		major, minor, OSFUI::API::kBridgeAPIMajor, OSFUI::API::kBridgeAPIMinor);
	return &OSFUI::API::BridgeApi::Get();
}
