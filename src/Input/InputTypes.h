#pragma once

namespace OSFUI
{
	// Window keyboard events carry VK codes; mouse buttons and wheel route separately from WM_INPUT.
	using KeyCode = std::uint32_t;

	// Persist binding identity as physical DIK scan codes; localized keycaps are display-only.
	using ScanCode = std::uint16_t;

	inline constexpr ScanCode kInvalidScanCode = 0;

}
