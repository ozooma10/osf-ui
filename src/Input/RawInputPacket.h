#pragma once

#include <Windows.h>
#include <cstddef>

namespace OSFUI
{
	// GetRawInputData returns the packet length, not the capacity of RAWINPUT's
	// union. On x64 a keyboard packet is 40 bytes while RAWINPUT is 48 bytes.
	inline bool IsCompleteRawKeyboard(const RAWINPUT& a_raw, UINT a_bytes)
	{
		constexpr auto required = offsetof(RAWINPUT, data) + sizeof(RAWKEYBOARD);
		return a_bytes >= required && a_bytes <= sizeof(RAWINPUT) &&
			a_raw.header.dwType == RIM_TYPEKEYBOARD &&
			a_raw.header.dwSize >= required && a_raw.header.dwSize <= a_bytes;
	}
}
