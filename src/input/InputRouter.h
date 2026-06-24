#pragma once

#include "input/InputTypes.h"

namespace PrismaSF
{
	// Input fan-in point. NOTHING CALLS THIS YET: no Starfield/SFSE input
	// event source has been identified, and this project does not install raw
	// Win32 hooks to fake one (see docs/reverse-engineering-notes.md).
	//
	// Once a real event source exists, it should:
	//   1. call these On* methods from whatever thread the game delivers
	//      input on (re-check thread-safety then),
	//   2. let the router decide between "toggle overlay", "forward to web
	//      view", or "pass through to game".
	class InputRouter
	{
	public:
		// a_toggleVisibility is the toggle path: invoked when the configured
		// toggle key is seen (and when ESC is pressed while captured).
		void Configure(KeyCode a_toggleKey, std::function<void()> a_toggleVisibility);

		// Wires keyboard routing into the web view. a_isCaptured tells the
		// router whether the overlay currently owns input (visible + capture
		// enabled); a_routeKey delivers a key transition to the view. Both are
		// optional — without them the router only drives the toggle path.
		void SetWebRouting(std::function<bool()> a_isCaptured,
			std::function<void(KeyCode, bool)> a_routeKey);

		void OnKeyDown(KeyCode a_key);
		void OnKeyUp(KeyCode a_key);
		void OnMouseMove(float a_x, float a_y);
		void OnMouseButton(MouseButton a_button, bool a_pressed);
		void OnTextInput(std::string_view a_utf8);

	private:
		[[nodiscard]] bool Captured() const { return _isCaptured && _isCaptured(); }

		KeyCode                            _toggleKey{ 0 };
		std::function<void()>              _toggleVisibility;
		std::function<bool()>              _isCaptured;
		std::function<void(KeyCode, bool)> _routeKey;
	};
}
