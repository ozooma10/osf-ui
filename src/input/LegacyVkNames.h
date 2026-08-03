#pragma once

#include <cstdint>
#include <string_view>

namespace OSFUI::Legacy
{
	// FROZEN copy of the pre-2.x name resolver, when key names were anchored
	// to Windows virtual-key codes instead of physical scan codes. It exists
	// only so the one-time values migration (SettingsStore, $formatVersion
	// 1 -> 2) can interpret names saved by older builds under the layout
	// active at migration time. Do not extend, do not fix, do not reuse —
	// new code resolves names through OSFUI::ResolveKeyName (scan codes).
	// Returns 0 when the name does not resolve; never logs.
	[[nodiscard]] std::uint32_t ResolveKeyNameVk(std::string_view a_name);
}
