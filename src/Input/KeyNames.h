#pragma once

#include "Input/InputTypes.h"

#include <string>
#include <string_view>

namespace OSFUI
{
	// Resolve a layout-independent US-reference key name to its physical DIK scan code.
	[[nodiscard]] ScanCode ResolveKeyName(std::string_view a_name);

	// Return the canonical round-trippable config name, or empty when unnameable.
	[[nodiscard]] std::string KeyName(ScanCode a_scan);

	namespace Legacy
	{
		// Frozen pre-2.0 VK resolver used only to migrate saved keys under the active layout.
		[[nodiscard]] std::uint32_t ResolveKeyNameVk(std::string_view a_name);
	}
}
