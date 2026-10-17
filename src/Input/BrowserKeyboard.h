#pragma once

#include <cstdint>
#include <string>
#include <string_view>

namespace osfui::browser::msg { struct Keyboard; }

namespace OSFUI
{
	osfui::browser::msg::Keyboard BrowserKeyboardEvent(std::uint32_t a_vk, bool a_down, bool a_system, std::intptr_t a_lparam);
	// Unmodified navigation key (arrows, Enter, Escape) with no window message behind it.
	osfui::browser::msg::Keyboard BrowserNavigationKeyEvent(std::uint32_t a_vk, bool a_down);
}
