#include "api/BridgeApi.h"

extern "C" __declspec(dllexport)
OSFUI::API::IOSFUIBridge* OSFUI_RequestBridge(std::uint32_t a_abiVersion) noexcept
{
	// MAJOR must match exactly. MINOR may differ
	if ((a_abiVersion >> 16) != OSFUI::API::kBridgeAPIMajor) {
		REX::WARN("OSFUI_RequestBridge: ABI major mismatch (caller major {}, ours {}) — returning null",
			a_abiVersion >> 16, OSFUI::API::kBridgeAPIMajor);
		return nullptr;
	}
	return &OSFUI::API::BridgeApi::Get();
}
