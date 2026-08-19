#include <intrin.h>  // _ReturnAddress — attributes a refusal to the calling plugin

#include "OSFUI_API.h"       // IOSFUIBridge, kBridgeAPIMajor
#include "API/BridgeApi.h"   // BridgeApi::Get()
#include "Platform/WindowsPlatform.h"

// ABI 1.x is append-only; callers feature-gate appended vmethods by the returned minor version.
extern "C" __declspec(dllexport) OSFUI::API::IOSFUIBridge* OSFUI_RequestBridge(std::uint32_t a_abiVersion) noexcept
{
	const auto major = a_abiVersion >> 16;
	const auto minor = a_abiVersion & 0xFFFFu;
	if (major != OSFUI::API::kBridgeAPIMajor) {
		OSFUI::API::BridgeApi::Get().NoteUnsupportedApiCaller(
			OSFUI::Platform::ModuleNameForAddress(_ReturnAddress()), major, minor);
		return nullptr;
	}
	// Newer callers are valid when they feature-gate tail vmethods with GetInterfaceVersion.
	REX::INFO("BridgeApi: bridge vended (caller ABI {}.{}, runtime ABI {}.{})",
		major, minor, OSFUI::API::kBridgeAPIMajor, OSFUI::API::kBridgeAPIMinor);
	return &OSFUI::API::BridgeApi::Get();
}
