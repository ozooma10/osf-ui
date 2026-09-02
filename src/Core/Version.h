#pragma once

#include <cstdint>

namespace OSFUI
{
	inline constexpr const char* kPluginName = "OSF UI";
	inline constexpr const char* kOsfuiReleaseVersion = "2.0.0";
	// Numeric release parts used by the Papyrus version surface.
	inline constexpr std::uint32_t kOsfuiReleaseVersionMajor = 2;
	inline constexpr std::uint32_t kOsfuiReleaseVersionMinor = 0;
	inline constexpr std::uint32_t kOsfuiReleaseVersionPatch = 0;

	inline constexpr const char* kBridgeProtocolVersion = "2.0";

	// Addon data root: Data/SFSE/Plugins/OSF/UI/.
	inline constexpr const char* kDataFolderName = "OSF/UI";
}
