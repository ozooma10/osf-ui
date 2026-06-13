#include "input/InputRouter.h"

#include <cctype>

#include "core/Log.h"

namespace SWUI
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

		// Keyboard ButtonEvents carry Windows VK codes (see InputTypes.h for
		// the in-game proof), so names resolve to VK values.

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
		};
		for (const auto& key : kNamedKeys) {
			if (equalsIgnoreCase(key.name, a_name)) {
				return key.vk;
			}
		}

		REX::WARN("InputRouter: could not resolve key name '{}'; toggle key disabled", a_name);
		return kInvalidKeyCode;
	}

	namespace
	{
		constexpr KeyCode kVkEscape = 0x1B;
	}

	void InputRouter::Configure(KeyCode a_toggleKey, std::function<void()> a_toggleVisibility)
	{
		_toggleKey = a_toggleKey;
		_toggleVisibility = std::move(a_toggleVisibility);
	}

	void InputRouter::SetWebRouting(std::function<bool()> a_isCaptured,
		std::function<void(KeyCode, bool)> a_routeKey)
	{
		_isCaptured = std::move(a_isCaptured);
		_routeKey = std::move(a_routeKey);
	}

	void InputRouter::OnKeyDown(KeyCode a_key)
	{
		// Toggle path: fed by UiInputHook when inputSource="ui". Handled before
		// capture so the toggle key always works, even while the overlay owns
		// input. ESC also closes the overlay when captured.
		const bool captured = Captured();
		if (_toggleVisibility) {
			if (_toggleKey != kInvalidKeyCode && a_key == _toggleKey) {
				_toggleVisibility();
				return;
			}
			if (captured && a_key == kVkEscape) {
				_toggleVisibility();
				return;
			}
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

	void InputRouter::OnMouseMove(float, float)
	{
		// Intentionally silent: would fire every frame once wired.
	}

	void InputRouter::OnMouseButton(MouseButton a_button, bool a_pressed)
	{
		if (Log::DevMode()) {
			REX::DEBUG("InputRouter: OnMouseButton({}, {})", static_cast<int>(a_button), a_pressed);
		}
	}

	void InputRouter::OnTextInput(std::string_view a_utf8)
	{
		if (Log::DevMode()) {
			REX::DEBUG("InputRouter: OnTextInput('{}')", a_utf8);
		}
	}
}
