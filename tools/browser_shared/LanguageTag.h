#pragma once

#include <string>
#include <string_view>

namespace osfui::browser::LanguageTag
{
	// Lower-cased sLanguage:General ("en", "ptbr", "zhhans") -> BCP-47. Unknown tag-safe codes pass through;
	// anything else returns empty (keep the browser default).
	inline std::string FromGameLanguage(std::string_view a_game)
	{
		std::string code;
		code.reserve(a_game.size());
		for (const char c : a_game) {
			if (c >= 'A' && c <= 'Z') {
				code.push_back(static_cast<char>(c + ('a' - 'A')));
			} else if ((c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '-') {
				code.push_back(c);
			} else {
				return {};
			}
		}
		if (code.empty() || code.size() > 32) {
			return {};
		}
		if (code == "zhhans") return "zh-Hans";
		if (code == "zhhant") return "zh-Hant";
		if (code.find('-') != std::string::npos) {
			return code;
		}
		if (code.size() == 4) {
			// Language plus region packed together: "ptbr" -> "pt-BR", "esmx" -> "es-MX".
			std::string tag = code.substr(0, 2) + "-";
			for (const char c : code.substr(2)) {
				tag.push_back(static_cast<char>(c >= 'a' && c <= 'z' ? c - ('a' - 'A') : c));
			}
			return tag;
		}
		return code;
	}
}
