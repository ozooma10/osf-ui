#pragma once

#include <algorithm>
#include <string_view>

namespace OSFUI
{
	// CSS hexadecimal color accepted by settings and view manifests.
	[[nodiscard]] inline bool IsHexColor(std::string_view a_value)
	{
		if ((a_value.size() != 7 && a_value.size() != 9) || a_value.front() != '#') return false;
		return std::ranges::all_of(a_value.substr(1), [](unsigned char a_character) {
			return (a_character >= '0' && a_character <= '9') || (a_character >= 'a' && a_character <= 'f') || (a_character >= 'A' && a_character <= 'F');
		});
	}
}
