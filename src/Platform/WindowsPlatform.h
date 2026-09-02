#pragma once

// Shared Win32 adapters for input translation and loaded-module facts.

namespace OSFUI::Platform
{
	// Map a VK to current-layout DIK for synthetic-input fallback.
	[[nodiscard]] std::uint32_t VkToDirectInputScan(std::uint32_t a_vk);

	// Return only the owning module's filename so diagnostics never expose the player's full path.
	[[nodiscard]] std::string ModuleNameForAddress(const void* a_address);
}
