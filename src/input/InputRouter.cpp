#include "input/InputRouter.h"

#include <cctype>

#include "core/Log.h"

namespace OSFUI
{
	KeyCode ResolveKeyName(std::string_view a_name)
	{
		if (a_name.empty()) {
			return kInvalidKeyCode;
		}

		const auto equalsIgnoreCase = [](std::string_view a_lhs, std::string_view a_rhs) {
			return std::ranges::equal(a_lhs, a_rhs, [](unsigned char l, unsigned char r) {
				return std::tolower(l) == std::tolower(r);
			});
		};

		// Keyboard ButtonEvents carry Windows VK codes (proof in InputTypes.h),
		// so names resolve to VK values.

		// F1-F24: VK_F1 (0x70) .. VK_F24 (0x87) are contiguous.
		if (a_name.size() >= 2 && (a_name[0] == 'F' || a_name[0] == 'f')) {
			int n = 0;
			if (std::from_chars(a_name.data() + 1, a_name.data() + a_name.size(), n).ec == std::errc{} &&
				n >= 1 && n <= 24) {
				return 0x70 + static_cast<KeyCode>(n - 1);
			}
		}

		// Single letter/digit: VK code == uppercase ASCII value.
		if (a_name.size() == 1 && std::isalnum(static_cast<unsigned char>(a_name[0]))) {
			return static_cast<KeyCode>(std::toupper(static_cast<unsigned char>(a_name[0])));
		}

		struct NamedKey
		{
			std::string_view name;
			KeyCode          vk;
		};
		static constexpr NamedKey kNamedKeys[] = {
			{ "Space", 0x20 }, { "Enter", 0x0D }, { "Return", 0x0D }, { "Tab", 0x09 },
			{ "Escape", 0x1B }, { "Backspace", 0x08 }, { "Insert", 0x2D }, { "Delete", 0x2E },
			{ "Home", 0x24 }, { "End", 0x23 }, { "PageUp", 0x21 }, { "PageDown", 0x22 },
			{ "Up", 0x26 }, { "Down", 0x28 }, { "Left", 0x25 }, { "Right", 0x27 },
			{ "CapsLock", 0x14 }, { "NumLock", 0x90 }, { "ScrollLock", 0x91 }, { "Pause", 0x13 },
			{ "LShift", 0xA0 }, { "RShift", 0xA1 }, { "LCtrl", 0xA2 }, { "RCtrl", 0xA3 },
			{ "LAlt", 0xA4 }, { "RAlt", 0xA5 },
			// Console/grave key (VK_OEM_3 on US layouts). Aliases for the same VK.
			{ "Grave", 0xC0 }, { "Tilde", 0xC0 }, { "Backtick", 0xC0 }, { "Console", 0xC0 },
			// OEM punctuation, layout-dependent like Grave above: these are the
			// US ANSI meanings of each VK. On a German layout VK_OEM_1 is 'ö',
			// not ';', but the name (and the on-screen board) still says
			// Semicolon — names are persisted in config JSON and must mean the
			// same key on every machine that loads it; only the printed keycap
			// differs (see the note on KeyName below). First spelling per VK is
			// canonical (KeyName returns it); the rest are input aliases so
			// hand-edited configs and schema defaults work.
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
		for (const auto& key : kNamedKeys) {
			if (equalsIgnoreCase(key.name, a_name)) {
				return key.vk;
			}
		}

		// Callers vary (toggle key, hotkey bindings, conflict grouping): an
		// unresolvable name simply does not bind.
		REX::WARN("InputRouter: could not resolve key name '{}'", a_name);
		return kInvalidKeyCode;
	}

	std::string KeyName(KeyCode a_vk)
	{
		// F1-F24 (contiguous from VK_F1 = 0x70).
		if (a_vk >= 0x70 && a_vk <= 0x87) {
			return "F" + std::to_string(a_vk - 0x70 + 1);
		}
		// Digits 0-9 (VK 0x30-0x39) and letters A-Z (VK 0x41-0x5A) are their ASCII.
		if ((a_vk >= 0x30 && a_vk <= 0x39) || (a_vk >= 0x41 && a_vk <= 0x5A)) {
			return std::string(1, static_cast<char>(a_vk));
		}
		// Canonical (first) name per VK; aliases like Return/Tilde resolve back
		// to Enter/Grave. Keep in lockstep with ResolveKeyName's kNamedKeys.
		switch (a_vk) {
		case 0x20: return "Space";
		case 0x0D: return "Enter";
		case 0x09: return "Tab";
		case 0x1B: return "Escape";
		case 0x08: return "Backspace";
		case 0x2D: return "Insert";
		case 0x2E: return "Delete";
		case 0x24: return "Home";
		case 0x23: return "End";
		case 0x21: return "PageUp";
		case 0x22: return "PageDown";
		case 0x26: return "Up";
		case 0x28: return "Down";
		case 0x25: return "Left";
		case 0x27: return "Right";
		case 0x14: return "CapsLock";
		case 0x90: return "NumLock";
		case 0x91: return "ScrollLock";
		case 0x13: return "Pause";
		case 0xA0: return "LShift";
		case 0xA1: return "RShift";
		case 0xA2: return "LCtrl";
		case 0xA3: return "RCtrl";
		case 0xA4: return "LAlt";
		case 0xA5: return "RAlt";
		case 0xC0: return "Grave";
		// OEM punctuation — canonical spelling per VK, US ANSI; see the layout
		// note on kNamedKeys.
		case 0xBD: return "Minus";
		case 0xBB: return "Equals";
		case 0xDB: return "LBracket";
		case 0xDD: return "RBracket";
		case 0xDC: return "Backslash";
		case 0xBA: return "Semicolon";
		case 0xDE: return "Apostrophe";
		case 0xBC: return "Comma";
		case 0xBE: return "Period";
		case 0xBF: return "Slash";
		default: return {};
		}
	}

	namespace
	{
		constexpr KeyCode kVkEscape = 0x1B;
	}

	void InputRouter::Configure(KeyCode a_toggleKey, std::function<void()> a_onToggle,
		std::function<void()> a_onBack)
	{
		_toggleKey = a_toggleKey;
		_onToggle = std::move(a_onToggle);
		_onBack = std::move(a_onBack);
	}

	void InputRouter::SetWebRouting(std::function<bool()> a_isCaptured,
		std::function<void(KeyCode, bool)> a_routeKey)
	{
		_isCaptured = std::move(a_isCaptured);
		_routeKey = std::move(a_routeKey);
	}

	void InputRouter::OnKeyDown(KeyCode a_key)
	{
		// Fed by the WndProc hook. Toggle is handled before capture so it works
		// even while the overlay owns input, and is a distinct intent from a
		// captured Esc (Esc = back: close the top menu, or delegate to a
		// back-owning view via osfui.handleBack). Both are consumed here so the
		// key never also routes into the view as a plain keystroke.
		const bool captured = Captured();
		if (_toggleKey != kInvalidKeyCode && a_key == _toggleKey) {
			if (_onToggle) {
				_onToggle();
			}
			return;
		}
		if (captured && a_key == kVkEscape) {
			if (_onBack) {
				_onBack();
			}
			return;
		}

		if (captured && _routeKey) {
			_routeKey(a_key, true);
			return;
		}
		if (Log::DevMode()) {
			REX::DEBUG("InputRouter: OnKeyDown({}) (overlay not capturing — passed to game)", a_key);
		}
	}

	void InputRouter::OnKeyUp(KeyCode a_key)
	{
		if (Captured() && _routeKey) {
			_routeKey(a_key, false);
			return;
		}
		if (Log::DevMode()) {
			REX::DEBUG("InputRouter: OnKeyUp({})", a_key);
		}
	}
}
