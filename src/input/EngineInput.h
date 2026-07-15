#pragma once

#include <cstdint>

namespace OSFUI
{
	// XInput wButtons masks — a gamepad ButtonEvent's idCode carries these
	// (0x1000 = A, proven in-game 2026-07-02).
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

	// Level-2 engine-routed input (config `engineInput`, on by default): a tap
	// on the engine's per-menu input dispatch that brings GAMEPAD input — which
	// the WndProc never sees — into the runtime. Proven contract (OSF RE module
	// ui.menu_input, 1.16.244): menus in the active array receive input through
	// the BSInputEventUser subobject at IMenu+0x10 — UI::PerformInputProcessing
	// walks the array top-down per event and dispatches by type to the receiver
	// vtable slots (1 ShouldHandleEvent, 4 thumbstick, 5 cursorMove, 6 mouseMove,
	// 7 char, 8 button; base slot 9 stays = held/release admission). Dispatch
	// arrives on a frame-worker THREAD POOL, so the thunks only bump atomic
	// counters and a small ring — no allocation, no game calls.
	//
	// The tap does NOT mark events handled: the WndProc path stays authoritative
	// for keyboard/mouse (no double input), and while the overlay captures, the
	// WndProc swallow starves the engine of keyboard/mouse — so in practice the
	// tap sees GAMEPAD events only, which is exactly the point of Level 2. The
	// queued gamepad edges + sticks below are drained on the main thread by
	// Runtime::DrainEngineInput and routed into the web view (nav/activate/close/
	// scroll) plus raw `ui.gamepad` events. Delivery + routing verified in-game
	// on 1.16.244 with a controller (2026-07-02).
	//
	// Stability note: the +0x10 vtable copy carries its RTTI COL at [-1] — the
	// same lesson as the primary-vtable copy (a COL-less copy AVs the first
	// time the engine dynamic_casts through it).
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
		// queue + zeroes the sticks so a released stick can't leak into the next
		// session.
		static void LogSessionSummary();

		// ---- Increment 3: gamepad routing (main thread) ----
		// GAMEPAD ONLY. Keyboard/mouse stay on the WndProc path by design (the
		// hybrid: text/IME + cursor position were always going to live there).
		// Populated by the receiver thunks on engine worker threads; drained by
		// Runtime on the game main thread.

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
		// (Fixed 64-slot ring — overflow drops the oldest edge.)
		static bool PollGamepadButton(GamepadButtonEdge& a_out);
		// Latest stick deflection (raw, roughly -1..1 per axis). Reports zero
		// when the last thumbstick dispatch is older than ~150ms — a staleness
		// guard, because it is unproven whether the engine sends a final
		// zero-deflection event on release or simply stops dispatching (the
		// latter would otherwise leave the default nav auto-repeating forever).
		[[nodiscard]] static GamepadSticks GetSticks();

		// Raw passthrough mode: when set, Runtime skips the default mapping
		// (nav/activate/close/scroll) and only forwards raw `ui.gamepad` events,
		// so a page can own the gamepad fully (e.g. stick-driven camera orbit)
		// without the defaults fighting it. Toggled via the `osfui.gamepadRaw`
		// bridge command. PER-SESSION: Runtime resets it to off on every overlay
		// close, so a page must re-assert it on each ui.visibility show — a
		// stale grant would otherwise leave default nav dead for the NEXT menu.
		static void SetRawMode(bool a_raw);
		[[nodiscard]] static bool IsRawMode();
	};
}
