#pragma once

namespace OSFUI
{
	// Subclasses the game's main window procedure (SetWindowLongPtr on the
	// game HWND, not a global SetWindowsHookEx) to intercept input at the OS
	// message boundary. This is the only point that can block gameplay input
	// (movement + camera/mouse-look): those read the raw WM_INPUT stream
	// directly, so blocking the engine's UI input sink is not enough (proven
	// in-game 2026-06-12 — see docs/reverse-engineering-notes.md §3).
	//
	// While the overlay owns input (Runtime::IsInputCaptured):
	//   - keyboard messages are routed into the web view and consumed,
	//   - the toggle key still toggles,
	//   - all mouse/raw-input messages are consumed so the game freezes,
	//   - the mouse drives the view, always routed from raw WM_INPUT (the
	//     game's raw-input registration suppresses the WM_MOUSE* stream, so
	//     it is the only source). Default (config.hardwareCursor): the OS
	//     pointer is shown (input/HardwareCursor — zero-lag,
	//     hardware-composited) and its live position (GetCursorPos) is the
	//     routed position. Fallback (hardwareCursor=false): raw deltas drive
	//     the runtime's invisible virtual cursor instead.
	// When not capturing, every message passes through unchanged except the
	// toggle key (consumed so it never reaches the game).
	namespace OverlayInputHook
	{
		// Private message posted by a renderer worker when native child focus
		// must return to Starfield's window-message thread.
		inline constexpr std::uint32_t kRestoreGameFocusMessage = 0x8049;
		// Wake the game window thread after the main-thread menu policy changes.
		// Its WndProc owns ShowCursor/ClipCursor state, which must be applied even
		// if native focus moves to the WebView before another input packet arrives.
		inline constexpr std::uint32_t kRefreshInputStateMessage = 0x804A;
		// Finds the game's main top-level window for the current process and
		// installs the WndProc subclass. Safe to call once game UI exists
		// (kPostPostDataLoad). Returns false (and logs) if no window is found
		// or the swap fails. One-way: never un-subclassed (other overlays may
		// chain on the same window).
		bool Install();
		void RequestStateRefresh();
	}
}
