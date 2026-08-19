#pragma once

// Shared Win32 adapters for keyboard-layout and loaded-module facts.

#include "Input/KeyLabels.h"

namespace OSFUI::Platform
{
	// Read keycap facts from the game-window thread's current layout without mutating dead-key state.
	[[nodiscard]] KeyLabelSource MakeKeyLabelSource(void* a_gameWindow);

	// Map a VK to current-layout DIK for synthetic-input fallback and pre-2.x migration.
	[[nodiscard]] std::uint32_t VkToDirectInputScan(std::uint32_t a_vk);

	// Return only the owning module's filename so diagnostics never expose the player's full path.
	[[nodiscard]] std::string ModuleNameForAddress(const void* a_address);
}
