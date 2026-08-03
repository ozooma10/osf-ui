#include "input/KeyLabels.h"

#include <cctype>

namespace OSFUI
{
	namespace
	{
		struct FixedLabel
		{
			std::string_view name;
			std::string_view english;
		};

		// Non-printing keys: label from our own short forms (localizable via
		// chrome.keys.<Name> catalog addresses), never from the layout DLL —
		// the layout's language is not the player's UI language, and its
		// spellings ("UMSCHALT", "RÜCK") are inconsistent across layouts. The
		// English defaults match the board's historical keycaps.
		constexpr FixedLabel kFixedLabels[] = {
			{ "Escape", "Esc" }, { "Backspace", "Bksp" }, { "Tab", "Tab" },
			{ "CapsLock", "Caps" }, { "Enter", "Enter" }, { "Space", "Space" },
			{ "LShift", "Shift" }, { "RShift", "Shift" },
			{ "LCtrl", "Ctrl" }, { "RCtrl", "Ctrl" },
			{ "LAlt", "Alt" }, { "RAlt", "Alt" },
			{ "Insert", "Ins" }, { "Delete", "Del" },
			{ "Home", "Home" }, { "End", "End" },
			{ "PageUp", "PgUp" }, { "PageDown", "PgDn" },
			{ "Up", "↑" }, { "Down", "↓" }, { "Left", "←" }, { "Right", "→" },
			{ "NumLock", "Num" }, { "ScrollLock", "ScrLk" }, { "Pause", "Pause" },
			{ "PrintScreen", "PrtSc" }, { "Apps", "Menu" },
			{ "LWin", "Win" }, { "RWin", "Win" },
			{ "NumpadEnter", "Num Enter" }, { "NumpadDivide", "Num /" },
			{ "NumpadMultiply", "Num *" }, { "NumpadSubtract", "Num -" },
			{ "NumpadAdd", "Num +" }, { "NumpadDecimal", "Num ." },
			{ "Numpad0", "Num 0" }, { "Numpad1", "Num 1" }, { "Numpad2", "Num 2" },
			{ "Numpad3", "Num 3" }, { "Numpad4", "Num 4" }, { "Numpad5", "Num 5" },
			{ "Numpad6", "Num 6" }, { "Numpad7", "Num 7" }, { "Numpad8", "Num 8" },
			{ "Numpad9", "Num 9" },
		};

		const FixedLabel* FindFixed(std::string_view a_name)
		{
			for (const auto& fixed : kFixedLabels) {
				if (fixed.name == a_name) {
					return &fixed;
				}
			}
			return nullptr;
		}

		bool IsFunctionKeyName(std::string_view a_name)
		{
			return a_name.size() >= 2 && a_name[0] == 'F' &&
			       std::isdigit(static_cast<unsigned char>(a_name[1]));
		}
	}

	KeyLabels BuildKeyLabels(const KeyLabelSource& a_source, const KeyTextResolver& a_text)
	{
		KeyLabels out;
		if (a_source.layoutTag) {
			out.layout = a_source.layoutTag();
		}
		for (unsigned code = 1; code <= 0xFF; ++code) {
			const auto scan = static_cast<ScanCode>(code);
			auto name = KeyName(scan);
			if (name.empty()) {
				continue;  // not a nameable key
			}
			std::string label;
			if (const auto* fixed = FindFixed(name)) {
				const auto address = "chrome.keys." + name;
				label = a_text ? a_text(address, fixed->english) : std::string(fixed->english);
			} else if (IsFunctionKeyName(name)) {
				label = name;  // F1-F24: universal, never a glyph
			} else {
				// Printable key: the layout's keycap glyph, with the layout-DLL
				// name and finally the key name as fallbacks — never empty.
				if (a_source.glyph) {
					label = a_source.glyph(scan);
				}
				if (label.empty() && a_source.layoutName) {
					label = a_source.layoutName(scan);
				}
				if (label.empty()) {
					label = name;
				}
			}
			out.labels.emplace_back(std::move(name), std::move(label));
		}
		return out;
	}
}
