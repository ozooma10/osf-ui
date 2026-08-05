#include "input/KeyNames.h"

#include "core/Log.h"
#include "core/StringUtil.h"

#include <cctype>
#include <charconv>

namespace OSFUI
{
	namespace
	{
		struct NamedScan
		{
			std::string_view name;
			ScanCode         code;
		};

		// The one source of truth for both directions. The first spelling per
		// code is canonical; later rows are aliases accepted from hand-edited
		// configs and W3C KeyboardEvent.code values.
		//
		// Codes are set-1 make codes in the DirectInput convention (0x80 | base
		// for 0xE0-prefixed extended keys), bit-identical to Starfield's
		// controlmap DIK tokens. A name denotes a physical US-reference position;
		// the keycap printed by the active layout is display data (KeyLabels).
		// Every name stays <=16 chars (the authoring key-value constraint).
		constexpr NamedScan kNamedScans[] = {
			{ "Escape", 0x01 },
			{ "1", 0x02 }, { "2", 0x03 }, { "3", 0x04 }, { "4", 0x05 }, { "5", 0x06 },
			{ "6", 0x07 }, { "7", 0x08 }, { "8", 0x09 }, { "9", 0x0A }, { "0", 0x0B },
			{ "Minus", 0x0C }, { "Hyphen", 0x0C }, { "Dash", 0x0C },
			{ "Equals", 0x0D }, { "Equal", 0x0D }, { "Plus", 0x0D },
			{ "Backspace", 0x0E }, { "Tab", 0x0F },
			{ "Q", 0x10 }, { "W", 0x11 }, { "E", 0x12 }, { "R", 0x13 }, { "T", 0x14 },
			{ "Y", 0x15 }, { "U", 0x16 }, { "I", 0x17 }, { "O", 0x18 }, { "P", 0x19 },
			{ "LBracket", 0x1A }, { "LeftBracket", 0x1A }, { "BracketLeft", 0x1A },
			{ "RBracket", 0x1B }, { "RightBracket", 0x1B }, { "BracketRight", 0x1B },
			{ "Enter", 0x1C }, { "Return", 0x1C },
			{ "LCtrl", 0x1D }, { "ControlLeft", 0x1D },
			{ "A", 0x1E }, { "S", 0x1F }, { "D", 0x20 }, { "F", 0x21 }, { "G", 0x22 },
			{ "H", 0x23 }, { "J", 0x24 }, { "K", 0x25 }, { "L", 0x26 },
			{ "Semicolon", 0x27 },
			{ "Apostrophe", 0x28 }, { "Quote", 0x28 },
			{ "Grave", 0x29 }, { "Tilde", 0x29 }, { "Backtick", 0x29 },
			{ "Backquote", 0x29 }, { "Console", 0x29 },
			{ "LShift", 0x2A }, { "ShiftLeft", 0x2A },
			{ "Backslash", 0x2B },
			{ "Z", 0x2C }, { "X", 0x2D }, { "C", 0x2E }, { "V", 0x2F }, { "B", 0x30 },
			{ "N", 0x31 }, { "M", 0x32 },
			{ "Comma", 0x33 },
			{ "Period", 0x34 }, { "Dot", 0x34 },
			{ "Slash", 0x35 },
			{ "RShift", 0x36 }, { "ShiftRight", 0x36 },
			{ "NumpadMultiply", 0x37 },
			{ "LAlt", 0x38 }, { "AltLeft", 0x38 },
			{ "Space", 0x39 },
			{ "CapsLock", 0x3A },
			{ "F1", 0x3B }, { "F2", 0x3C }, { "F3", 0x3D }, { "F4", 0x3E },
			{ "F5", 0x3F }, { "F6", 0x40 }, { "F7", 0x41 }, { "F8", 0x42 },
			{ "F9", 0x43 }, { "F10", 0x44 },
			{ "NumLock", 0x45 }, { "ScrollLock", 0x46 },
			{ "Numpad7", 0x47 }, { "Numpad8", 0x48 }, { "Numpad9", 0x49 },
			{ "NumpadSubtract", 0x4A },
			{ "Numpad4", 0x4B }, { "Numpad5", 0x4C }, { "Numpad6", 0x4D },
			{ "NumpadAdd", 0x4E },
			{ "Numpad1", 0x4F }, { "Numpad2", 0x50 }, { "Numpad3", 0x51 },
			{ "Numpad0", 0x52 }, { "NumpadDecimal", 0x53 },
			{ "IntlBackslash", 0x56 }, { "Oem102", 0x56 },
			{ "F11", 0x57 }, { "F12", 0x58 },
			{ "F13", 0x64 }, { "F14", 0x65 }, { "F15", 0x66 }, { "F16", 0x67 },
			{ "F17", 0x68 }, { "F18", 0x69 }, { "F19", 0x6A }, { "F20", 0x6B },
			{ "F21", 0x6C }, { "F22", 0x6D }, { "F23", 0x6E },
			{ "IntlRo", 0x73 },
			{ "F24", 0x76 },
			{ "IntlYen", 0x7D },
			{ "NumpadEnter", 0x9C },
			{ "RCtrl", 0x9D }, { "ControlRight", 0x9D },
			{ "NumpadDivide", 0xB5 },
			{ "PrintScreen", 0xB7 }, { "PrtScn", 0xB7 },
			{ "RAlt", 0xB8 }, { "AltRight", 0xB8 },
			{ "Pause", 0xC5 },
			{ "Home", 0xC7 },
			{ "Up", 0xC8 }, { "ArrowUp", 0xC8 },
			{ "PageUp", 0xC9 },
			{ "Left", 0xCB }, { "ArrowLeft", 0xCB },
			{ "Right", 0xCD }, { "ArrowRight", 0xCD },
			{ "End", 0xCF },
			{ "Down", 0xD0 }, { "ArrowDown", 0xD0 },
			{ "PageDown", 0xD1 },
			{ "Insert", 0xD2 }, { "Delete", 0xD3 },
			{ "LWin", 0xDB }, { "MetaLeft", 0xDB },
			{ "RWin", 0xDC }, { "MetaRight", 0xDC },
			{ "Apps", 0xDD }, { "ContextMenu", 0xDD },
		};

		struct LegacyNamedKey
		{
			std::string_view name;
			std::uint32_t    vk;
		};

		// Verbatim pre-2.0 VK-anchored names. Frozen for saved-value migration.
		constexpr LegacyNamedKey kLegacyNamedKeys[] = {
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

	ScanCode ResolveKeyName(std::string_view a_name)
	{
		if (a_name.empty()) {
			return kInvalidScanCode;
		}
		for (const auto& key : kNamedScans) {
			if (StringUtil::EqualsCaseInsensitiveAscii(key.name, a_name)) {
				return key.code;
			}
		}
		REX::WARN("KeyNames: could not resolve key name '{}'", a_name);
		return kInvalidScanCode;
	}

	std::string KeyName(ScanCode a_scan)
	{
		for (const auto& key : kNamedScans) {
			if (key.code == a_scan) {
				return std::string(key.name);
			}
		}
		return {};
	}

	namespace Legacy
	{
		std::uint32_t ResolveKeyNameVk(std::string_view a_name)
		{
			if (a_name.empty()) {
				return 0;
			}
			if (a_name.size() >= 2 && (a_name[0] == 'F' || a_name[0] == 'f')) {
				int n = 0;
				if (std::from_chars(a_name.data() + 1, a_name.data() + a_name.size(), n).ec == std::errc{} &&
					n >= 1 && n <= 24) {
					return 0x70 + static_cast<std::uint32_t>(n - 1);
				}
			}
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
}
