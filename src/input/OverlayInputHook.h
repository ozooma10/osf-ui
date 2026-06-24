#pragma once

namespace PrismaSF
{
	// Subclasses the game's main window procedure (SetWindowLongPtr on the
	// game HWND — NOT a global SetWindowsHookEx) to intercept input at the OS
	// message boundary. This is the only point that can block GAMEPLAY input
	// (movement + camera/mouse-look): those read the raw WM_INPUT stream
	// directly, so blocking the engine's UI input sink is not enough (proven
	// in-game 2026-06-12 — see docs/reverse-engineering-notes.md §3).
	//
	// While the overlay owns input (Runtime::IsInputCaptured):
	//   - keyboard messages are routed into the web view and consumed,
	//   - the toggle key still toggles,
	//   - all mouse/raw-input messages are consumed so the game freezes.
	// When not capturing, every message passes through unchanged except the
	// toggle key (consumed so it never reaches the game).
	//
	// Mouse routing + a visible cursor are a follow-up (Phase 4b); for now the
	// mouse is only BLOCKED while captured, not yet delivered to the view.
	namespace OverlayInputHook
	{
		// Finds the game's main top-level window for the current process and
		// installs the WndProc subclass. Safe to call once game UI exists
		// (kPostPostDataLoad). Returns false (and logs) if no window is found
		// or the swap fails. One-way: never un-subclassed (other overlays may
		// chain on the same window).
		bool Install();
	}
}
