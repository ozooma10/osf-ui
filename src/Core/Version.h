#pragma once

#include <array>
#include <charconv>
#include <cstdint>
#include <string_view>

namespace OSFUI
{
	inline constexpr const char* kPluginName = "OSF UI";
	inline constexpr const char* kOsfuiReleaseVersion = "2.0.0";
	// Numeric OSF UI release, exposed through OSFUIBridge::GetPluginVersion. Keep these in lockstep.
	inline constexpr std::uint32_t kOsfuiReleaseVersionMajor = 2;
	inline constexpr std::uint32_t kOsfuiReleaseVersionMinor = 0;
	inline constexpr std::uint32_t kOsfuiReleaseVersionPatch = 0;

	inline constexpr const char* kBridgeProtocolVersion = "2.0";

	inline std::optional<std::array<std::uint32_t, 3>> ParseDottedVersion(std::string_view a_text)
	{
		std::array<std::uint32_t, 3> parts{};
		for(auto& part : parts) {
			const auto dot = a_text.find('.');
			const auto token = a_text.substr(0, dot);
			const auto [end, error] = std::from_chars(token.data(), token.data() + token.size(), part);
			if(error != std::errc{} || end != token.data() + token.size()) {
				return std::nullopt;
			}
			if(dot == std::string_view::npos) {
				return parts;
			}
			a_text.remove_prefix(dot + 1);
		}
		return std::nullopt;
	}

	// Numeric form of the running OSF UI release, for targetVersion comparisons.
	inline constexpr std::array<std::uint32_t, 3> kOsfuiReleaseVersionParts{
		kOsfuiReleaseVersionMajor, kOsfuiReleaseVersionMinor, kOsfuiReleaseVersionPatch
	};

	inline bool IsPre2Target(std::string_view a_target)
	{
		const auto version = ParseDottedVersion(a_target);
		return version && (*version)[0] < 2;
	}

	inline bool IsTargetNewerThanInstalledRelease(std::string_view a_target)
	{
		const auto version = ParseDottedVersion(a_target);
		return version && kOsfuiReleaseVersionParts < *version;
	}

	// Name of the plugin data folder, resolved relative to the plugin DLL: Data/SFSE/Plugins/OSFUI/
	inline constexpr const char* kDataFolderName = "OSFUI";
}
