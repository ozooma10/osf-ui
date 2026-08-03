#include "input/LegacyVkNames.h"

#include <cctype>
#include <charconv>

#include "core/StringUtil.h"

namespace OSFUI::Legacy
{
	namespace
	{
		struct NamedKey
		{
			std::string_view name;
			std::uint32_t    vk;
		};

		// Verbatim pre-2.x kNamedKeys (VK-anchored, US ANSI meanings). Frozen.
		constexpr NamedKey kLegacyNamedKeys[] = {
			{ "Space", 0x20 }, { "Enter", 0x0D }, { "Return", 0x0D }, { "Tab", 0x09 },
			{ "Escape", 0x1B }, { "Backspace", 0x08 }, { "Insert", 0x2D }, { "Delete", 0x2E },
			{ "Home", 0x24 }, { "End", 0x23 }, { "PageUp", 0x21 }, { "PageDown", 0x22 },
			{ "Up", 0x26 }, { "Down", 0x28 }, { "Left", 0x25 }, { "Right", 0x27 },
			{ "CapsLock", 0x14 }, { "NumLock", 0x90 }, { "ScrollLock", 0x91 }, { "Pause", 0x13 },
			{ "LShift", 0xA0 }, { "RShift", 0xA1 }, { "LCtrl", 0xA2 }, { "RCtrl", 0xA3 },
			{ "LAlt", 0xA4 }, { "RAlt", 0xA5 },
			{ "Grave", 0xC0 }, { "Tilde", 0xC0 }, { "Backtick", 0xC0 }, { "Console", 0xC0 },
			{ "Minus", 0xBD }, { "Hyphen", 0xBD }, { "Dash", 0xBD },
			{ "Equals", 0xBB }, { "Equal", 0xBB }, { "Plus", 0xBB },
			{ "LBracket", 0xDB }, { "LeftBracket", 0xDB },
			{ "RBracket", 0xDD }, { "RightBracket", 0xDD },
			{ "Backslash", 0xDC },
			{ "Semicolon", 0xBA },
			{ "Apostrophe", 0xDE }, { "Quote", 0xDE },
			{ "Comma", 0xBC },
			{ "Period", 0xBE }, { "Dot", 0xBE },
			{ "Slash", 0xBF },
		};
	}

	std::uint32_t ResolveKeyNameVk(std::string_view a_name)
	{
		if (a_name.empty()) {
			return 0;
		}

		// F1-F24: VK_F1 (0x70) .. VK_F24 (0x87) are contiguous.
		if (a_name.size() >= 2 && (a_name[0] == 'F' || a_name[0] == 'f')) {
			int n = 0;
			if (std::from_chars(a_name.data() + 1, a_name.data() + a_name.size(), n).ec == std::errc{} &&
				n >= 1 && n <= 24) {
				return 0x70 + static_cast<std::uint32_t>(n - 1);
			}
		}

		// Single letter/digit: VK code == uppercase ASCII value.
		if (a_name.size() == 1 && std::isalnum(static_cast<unsigned char>(a_name[0]))) {
			return static_cast<std::uint32_t>(std::toupper(static_cast<unsigned char>(a_name[0])));
		}

		for (const auto& key : kLegacyNamedKeys) {
			if (StringUtil::EqualsCaseInsensitiveAscii(key.name, a_name)) {
				return key.vk;
			}
		}
		return 0;
	}
}
