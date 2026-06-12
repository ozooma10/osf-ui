#include "input/InputRouter.h"

#include <cctype>

#include "REX/W32/DINPUT.h"
#include "SFSE/InputMap.h"

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

		// F1-F12: layout-independent fast path (DIK codes are not contiguous
		// past F10).
		if (a_name.size() >= 2 && (a_name[0] == 'F' || a_name[0] == 'f')) {
			int n = 0;
			if (std::from_chars(a_name.data() + 1, a_name.data() + a_name.size(), n).ec == std::errc{}) {
				if (n >= 1 && n <= 10) {
					return REX::W32::DIK_F1 + static_cast<KeyCode>(n - 1);
				}
				if (n == 11) {
					return REX::W32::DIK_F11;
				}
				if (n == 12) {
					return REX::W32::DIK_F12;
				}
			}
		}

		// Everything else: match against the library's own key naming. Names
		// come from GetKeyNameText and therefore depend on keyboard layout.
		for (KeyCode code = 1; code < SFSE::InputMap::kMacro_NumKeyboardKeys; ++code) {
			if (const auto name = SFSE::InputMap::GetKeyboardKeyName(code);
				!name.empty() && equalsIgnoreCase(name, a_name)) {
				return code;
			}
		}

		REX::WARN("InputRouter: could not resolve key name '{}'; toggle key disabled", a_name);
		return kInvalidKeyCode;
	}

	void InputRouter::Configure(KeyCode a_toggleKey, std::function<void()> a_toggleVisibility)
	{
		_toggleKey = a_toggleKey;
		_toggleVisibility = std::move(a_toggleVisibility);
	}

	void InputRouter::OnKeyDown(KeyCode a_key)
	{
		// Toggle path: fed by UiInputHook when inputSource="ui".
		if (_toggleKey != kInvalidKeyCode && a_key == _toggleKey && _toggleVisibility) {
			_toggleVisibility();
			return;
		}
		if (Log::DevMode()) {
			REX::DEBUG("InputRouter: OnKeyDown({}) (not routed — no web focus model yet)", a_key);
		}
	}

	void InputRouter::OnKeyUp(KeyCode a_key)
	{
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
