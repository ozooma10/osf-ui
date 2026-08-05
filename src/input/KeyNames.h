#pragma once

#include "input/InputTypes.h"

#include <string>
#include <string_view>

namespace OSFUI
{
	// Resolve a config key name ("F10", "A", "Delete", ...) to its physical
	// scan code. Names denote positions on the US reference keyboard, exactly
	// like Starfield's controlmap DIK values, so they are layout-independent.
	// Returns kInvalidScanCode and logs when the name cannot be resolved.
	[[nodiscard]] ScanCode ResolveKeyName(std::string_view a_name);

	// Reverse of ResolveKeyName: a scan code -> its canonical config name.
	// Returns an empty string for an unnameable code. Round-trips:
	// ResolveKeyName(KeyName(scan)) == scan.
	[[nodiscard]] std::string KeyName(ScanCode a_scan);

	namespace Legacy
	{
		// Frozen pre-2.0 resolver used only by the one-time saved-key migration.
		// Older values were Windows-VK anchored; interpret them under the layout
		// active at migration time, then persist the physical name. Never extend
		// this table or use it for new bindings. Returns 0 without logging on miss.
		[[nodiscard]] std::uint32_t ResolveKeyNameVk(std::string_view a_name);
	}
}
