#pragma once

#include <cstdint>

namespace OSFUI::XInputButton
{
	// XInput wButtons masks; these numeric ids are also published by ui.gamepad.
	inline constexpr std::uint32_t kDPadUp = 0x0001;
	inline constexpr std::uint32_t kDPadDown = 0x0002;
	inline constexpr std::uint32_t kDPadLeft = 0x0004;
	inline constexpr std::uint32_t kDPadRight = 0x0008;
	inline constexpr std::uint32_t kStart = 0x0010;
	inline constexpr std::uint32_t kBack = 0x0020;
	inline constexpr std::uint32_t kLThumb = 0x0040;
	inline constexpr std::uint32_t kRThumb = 0x0080;
	inline constexpr std::uint32_t kLShoulder = 0x0100;
	inline constexpr std::uint32_t kRShoulder = 0x0200;
	inline constexpr std::uint32_t kA = 0x1000;
	inline constexpr std::uint32_t kB = 0x2000;
	inline constexpr std::uint32_t kX = 0x4000;
	inline constexpr std::uint32_t kY = 0x8000;
}
