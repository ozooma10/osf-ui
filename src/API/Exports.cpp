#include "OSFUI.h"
#include "API/BridgeApi.h"

extern "C" __declspec(dllexport) void* OSFUI_RequestAPI(
	std::uint32_t a_version, std::uint32_t* a_outVersion) noexcept
{
	if (a_outVersion) *a_outVersion = 0;
	if (a_version != OSFUI::API::kVersion) {
		REX::WARN("OSFUI_RequestAPI: refused version {:#x}; runtime is {:#x}",
			a_version, OSFUI::API::kVersion);
		return nullptr;
	}
	if (a_outVersion) *a_outVersion = OSFUI::API::kVersion;
	return static_cast<OSFUI::API::IUI*>(&OSFUI::API::BridgeApi::Get());
}
