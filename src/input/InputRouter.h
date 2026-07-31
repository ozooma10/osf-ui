#pragma once

#include "input/InputTypes.h"

#include <atomic>

namespace OSFUI
{
	// Keyboard decision point, fed by the WndProc subclass (OverlayInputHook →
	// Runtime::OnHostKey) on the window-message thread. Per key transition,
	// picks between toggle overlay, close top menu (Esc), route into the web
	// view, or pass to the game. Mouse does not come through here; text and IME
	// go directly to WebView2 through its focused native child window.
	class InputRouter
	{
	public:
		void Configure(KeyCode a_toggleKey, std::function<void()> a_onToggle, std::function<void()> a_onBack = {});
		void SetToggleKey(KeyCode a_toggleKey);

		// a_isCaptured: overlay currently owns input (visible + capture enabled).
		// Both are optional — without them the router only drives the toggle path.
		void SetWebRouting(std::function<bool()> a_isCaptured, std::function<void(KeyCode, bool)> a_routeKey);

		void OnKeyDown(KeyCode a_key);
		void OnKeyUp(KeyCode a_key);

	private:
		[[nodiscard]] bool Captured() const { return _isCaptured && _isCaptured(); }

		std::atomic<KeyCode>               _toggleKey{ kInvalidKeyCode };
		std::function<void()>              _onToggle;
		std::function<void()>              _onBack;
		std::function<bool()>              _isCaptured;
		std::function<void(KeyCode, bool)> _routeKey;
	};
}
