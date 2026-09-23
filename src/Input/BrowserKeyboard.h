#pragma once

#include <cstdint>
#include <string>
#include <string_view>

namespace osfui::wv2::msg { struct Keyboard; }

namespace OSFUI
{
	std::string BrowserInputUtf8(std::wstring_view a_text);
	osfui::wv2::msg::Keyboard BrowserKeyboardEvent(std::uint32_t a_vk, bool a_down,
		bool a_system, std::intptr_t a_lparam);
}
