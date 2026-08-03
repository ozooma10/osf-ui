#pragma once

#include "input/InputTypes.h"

namespace OSFUI
{
	// Builds the canonical ScanCode from the fields a keyboard message carries:
	// the raw make code (lParam bits 16-23) and the extended flag (bit 24),
	// folded into the DirectInput convention (0x80 | base for extended keys).
	//
	// Three keys need the VK because their raw fields collide or lie:
	//  * Pause is the E1-prefixed sequence E1 1D 45; messages report raw 0x45
	//    with the extended bit CLEAR — indistinguishable from NumLock by scan
	//    fields alone. DIK_PAUSE is 0xC5.
	//  * NumLock reports raw 0x45 too (some paths set the extended bit even
	//    though the wire code has no E0 prefix). DIK_NUMLOCK is 0x45.
	//  * PrintScreen (VK_SNAPSHOT) reports E0 37 but only ever surfaces as a
	//    key-up; normalize to DIK_SYSRQ 0xB7 regardless of message fields.
	//
	// Returns kInvalidScanCode when the message carried no scan code at all
	// (SendInput-synthesized input); the caller falls back to
	// Platform::VkToDirectInputScan.
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
