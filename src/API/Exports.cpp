#include "OSFUI_Views.h"
#include "API/BridgeApi.h"

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

extern "C" __declspec(dllexport) void* OSFUI_RequestViews(
	std::uint32_t a_version, std::uint32_t* a_outVersion) noexcept
{
	return RequestService("views", a_version, OSFUI::API::Views::kVersion,
		static_cast<OSFUI::API::Views::IViews*>(&OSFUI::API::BridgeApi::Get()), a_outVersion);
}
