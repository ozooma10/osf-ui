#include "input/InputRouter.h"

#include <cctype>

#include "core/Log.h"
#include "core/StringUtil.h"

namespace OSFUI
{
	namespace
	{
		struct NamedKey
		{
			std::string_view name;
			KeyCode          vk;
		};

		// The one source of truth for both directions (ResolveKeyName / KeyName).
		// The FIRST spelling per VK is canonical — KeyName returns it; the
		// remaining same-VK rows are input aliases so hand-edited configs and
		// schema defaults resolve.
		//
		// OEM punctuation is layout-dependent: these are the US ANSI meanings of
		// each VK. On a German layout VK_OEM_1 is 'ö', not ';', but the name (and
		// the on-screen board) still says Semicolon — names are persisted in config
		// JSON and must mean the same key on every machine that loads it; only the
		// printed keycap differs.
		constexpr NamedKey kNamedKeys[] = {
			{ "Space", 0x20 }, { "Enter", 0x0D }, { "Return", 0x0D }, { "Tab", 0x09 },
			{ "Escape", 0x1B }, { "Backspace", 0x08 }, { "Insert", 0x2D }, { "Delete", 0x2E },
			{ "Home", 0x24 }, { "End", 0x23 }, { "PageUp", 0x21 }, { "PageDown", 0x22 },
			{ "Up", 0x26 }, { "Down", 0x28 }, { "Left", 0x25 }, { "Right", 0x27 },
			{ "CapsLock", 0x14 }, { "NumLock", 0x90 }, { "ScrollLock", 0x91 }, { "Pause", 0x13 },
			{ "LShift", 0xA0 }, { "RShift", 0xA1 }, { "LCtrl", 0xA2 }, { "RCtrl", 0xA3 },
			{ "LAlt", 0xA4 }, { "RAlt", 0xA5 },
			// Console/grave key (VK_OEM_3 on US layouts). Aliases for the same VK.
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

	KeyCode ResolveKeyName(std::string_view a_name)
	{
		if (a_name.empty()) {
			return kInvalidKeyCode;
		}

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

		for (const auto& key : kNamedKeys) {
			if (StringUtil::EqualsCaseInsensitiveAscii(key.name, a_name)) {
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
		// Canonical (first) name per VK; aliases like Return/Tilde resolve back to
		// Enter/Grave. The first kNamedKeys row per VK is the canonical spelling,
		// so the two directions cannot drift.
		for (const auto& key : kNamedKeys) {
			if (key.vk == a_vk) {
				return std::string(key.name);
			}
		}
		return {};
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
