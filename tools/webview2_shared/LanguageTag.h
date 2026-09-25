#pragma once

#include <string>
#include <string_view>

namespace osfui::wv2::LanguageTag
{
	// Maps a Bethesda game language code (sLanguage:General, lower-cased: "en", "de", "ptbr", "zhhans") to the
	// BCP-47 tag WebView2 expects for its environment language. Unknown shapes pass through unchanged when they
	// are already tag-safe; anything else yields an empty string so the caller keeps the WebView2 default.
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
