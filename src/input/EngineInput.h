#pragma once

#include <cstdint>

namespace OSFUI
{
	// XInput wButtons masks — a gamepad ButtonEvent's idCode carries these
	// (0x1000 = A, confirmed in-game 2026-07-02).
	namespace XInputButton
	{
		inline constexpr std::uint32_t kDPadUp = 0x0001;
		inline constexpr std::uint32_t kDPadDown = 0x0002;
		inline constexpr std::uint32_t kDPadLeft = 0x0004;
		inline constexpr std::uint32_t kDPadRight = 0x0008;
		inline constexpr std::uint32_t kStart = 0x0010;
		inline constexpr std::uint32_t kBack = 0x0020;
		inline constexpr std::uint32_t kLThumb = 0x0040;
		inline constexpr std::uint32_t kRThumb = 0x0080;
		inline constexpr std::uint32_t kLShoulder = 0x0100;
		inline constexpr std::uint32_t kRShoulder = 0x0200;
		inline constexpr std::uint32_t kA = 0x1000;
		inline constexpr std::uint32_t kB = 0x2000;
		inline constexpr std::uint32_t kX = 0x4000;
		inline constexpr std::uint32_t kY = 0x8000;
	}

	// Engine-routed input (config `engineInput`, on by default): a tap on the
	// engine's per-menu input dispatch, bringing gamepad input — which the
	// WndProc never sees — into the runtime.
	//
	// Contract (OSF RE module ui.menu_input, 1.16.244): menus in the active array
	// receive input through the BSInputEventUser subobject at IMenu+0x10.
	// UI::PerformInputProcessing walks the array top-down per event, dispatching
	// by type to receiver vtable slots (1 ShouldHandleEvent, 4 thumbstick,
	// 5 cursorMove, 6 mouseMove, 7 char, 8 button; base slot 9 stays =
	// held/release admission). Dispatch arrives on a frame-worker thread pool, so
	// the thunks only bump atomic counters and a small ring — no allocation, no
	// game calls.
	//
	// The tap does not mark events handled, so WndProc stays authoritative for
	// keyboard/mouse (no double input); while the overlay captures, the WndProc
	// swallow starves the engine of keyboard/mouse, so the tap sees gamepad
	// events only. Queued edges + sticks below are drained on the main thread by
	// Runtime::DrainEngineInput and routed into the web view (nav/activate/
	// close/scroll) plus raw `ui.gamepad` events. Verified in-game on 1.16.244
	// with a controller (2026-07-02).
	//
	// The +0x10 vtable copy must carry its RTTI COL at [-1], as with the primary
	// vtable copy: a COL-less copy access-violates the first time the engine
	// dynamic_casts through it.
	class EngineInput
	{
	public:
		// Master switch, set once at init from config.engineInput.
		static void SetEnabled(bool a_enabled);
		[[nodiscard]] static bool IsEnabled();

		// Overwrite the +0x10 BSInputEventUser vptr of a freshly engine-built
		// focus-menu object with the patched copy. Called from the FocusMenu
		// creator; no-op unless enabled.
		static void InstallReceiver(void* a_menuObj);

		// One INFO line summarizing everything observed since the last call,
		// then reset. Runtime calls this on the focus-menu close edge so each
		// overlay session gets exactly one summary. Also clears the routing
		// queue and zeroes the sticks so a released stick can't leak into the
		// next session.
		static void LogSessionSummary();

		// Gamepad routing. Keyboard/mouse (text/IME + cursor position) stay on
		// the WndProc path. Populated by the receiver thunks on engine worker
		// threads; drained by Runtime on the game main thread.

		struct GamepadButtonEdge
		{
			std::uint32_t idCode{ 0 };  // XInputButton mask
			bool          down{ false };
		};
		struct GamepadSticks
		{
			float lx{ 0.0f }, ly{ 0.0f };  // left stick, per-axis (up = +y)
			float rx{ 0.0f }, ry{ 0.0f };  // right stick
		};

		// Pop one queued gamepad button edge; false when the queue is empty.
		// Fixed 64-slot ring — overflow drops the oldest edge.
		static bool PollGamepadButton(GamepadButtonEdge& a_out);
		// Latest stick deflection (raw, roughly -1..1 per axis). Reports zero
		// when the last thumbstick dispatch is older than ~150ms: it is unknown
		// whether the engine sends a final zero-deflection event on release or
		// just stops dispatching, and the latter would leave the default nav
		// auto-repeating forever.
		[[nodiscard]] static GamepadSticks GetSticks();

		// Raw passthrough mode: Runtime skips the default mapping
		// (nav/activate/close/scroll) and forwards only raw `ui.gamepad` events,
		// so a page can own the gamepad fully (e.g. stick-driven camera orbit).
		// Toggled via the `osfui.gamepadRaw` bridge command. Per-session:
		// Runtime resets it to off on every overlay close, so a page must
		// re-assert it on each ui.visibility show; a stale grant would leave
		// default nav dead for the next menu.
		static void SetRawMode(bool a_raw);
		[[nodiscard]] static bool IsRawMode();
	};
}
