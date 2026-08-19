#pragma once

#include "Input/InputTypes.h"

namespace OSFUI
{
	// Normalize WM key data to DIK, special-casing Pause, NumLock, and PrintScreen; zero scan uses the VK fallback.
	[[nodiscard]] constexpr ScanCode ComposeScanCode(std::uint32_t a_vk,
		std::uint8_t a_rawScan, bool a_extended) noexcept
	{
		constexpr std::uint32_t kVkPause = 0x13;
		constexpr std::uint32_t kVkSnapshot = 0x2C;
		constexpr std::uint32_t kVkNumLock = 0x90;

		switch (a_vk) {
		case kVkPause:
			return 0xC5;
		case kVkNumLock:
			return 0x45;
		case kVkSnapshot:
			return 0xB7;
		default:
			break;
		}
		if (a_rawScan == 0) {
			return kInvalidScanCode;
		}
		return static_cast<ScanCode>(a_extended ? (0x80u | a_rawScan) : a_rawScan);
	}
}
