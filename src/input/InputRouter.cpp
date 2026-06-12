#include "input/InputRouter.h"

#include "core/Log.h"

namespace SWUI
{
	void InputRouter::Configure(KeyCode a_toggleKey, std::function<void()> a_toggleVisibility)
	{
		_toggleKey = a_toggleKey;
		_toggleVisibility = std::move(a_toggleVisibility);
	}

	void InputRouter::OnKeyDown(KeyCode a_key)
	{
		// Placeholder toggle path. Works as soon as something feeds key events
		// into this router; nothing does yet.
		if (_toggleKey != 0 && a_key == _toggleKey && _toggleVisibility) {
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
