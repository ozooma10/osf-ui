#include "input/EngineInput.h"

#include "RE/B/BSInputEventUser.h"
#include "RE/IDs_VTABLE.h"

#include "core/Log.h"

#include <atomic>
#include <chrono>
#include <cstdint>
#include <format>
#include <mutex>
#include <string>

namespace OSFUI
{
	namespace
	{
		std::atomic_bool g_enabled{ false };

		// Patched +0x10 receiver vtable (copy of engine 475517): 10 slots
		// (0 dtor .. 9 Unk09) plus one leading slot for the engine's RTTI COL
		// (vtable[-1]), without which dynamic_cast through the copy fails — the
		// failure mode the primary-vtable copy hit on Route A.
		constexpr std::size_t kRecvSlots = 10;
		std::atomic_bool      g_recvBuilt{ false };
		void*                 g_recvStore[kRecvSlots + 1]{};
		void** const          g_recvVtable = &g_recvStore[1];

		// Observation state; thunks run on the engine's worker pool.
		std::atomic<std::uint32_t> g_shouldCalls{ 0 };
		std::atomic<std::uint32_t> g_buttons{ 0 };
		std::atomic<std::uint32_t> g_buttonsKeyboard{ 0 };
		std::atomic<std::uint32_t> g_buttonsMouse{ 0 };
		std::atomic<std::uint32_t> g_buttonsGamepad{ 0 };
		std::atomic<std::uint32_t> g_sticks{ 0 };
		std::atomic<std::uint32_t> g_chars{ 0 };
		std::atomic<std::uint32_t> g_mouseMoves{ 0 };
		std::atomic<std::uint32_t> g_cursorMoves{ 0 };

		// Small ring of the most recent button events for the summary line.
		struct ButtonRecord
		{
			std::int32_t  idCode{ -1 };
			std::uint32_t deviceType{ 0 };
			bool          down{ false };
		};
		constexpr std::size_t kRingSize = 8;
		std::mutex            g_ringMutex;  // leaf lock, thunk-side only
		ButtonRecord          g_ring[kRingSize]{};
		std::size_t           g_ringNext{ 0 };
		std::size_t           g_ringCount{ 0 };

		// Routing state (gamepad only). Button edges are queued worker-thread ->
		// main-thread drain in a fixed ring: no allocation on the engine's worker
		// threads, per the header's contract. Overflow drops the oldest edge,
		// harmless for the default mapping, which only acts on presses. Stick
		// deflection is latest-wins (atomic overwrite; only the current value
		// matters). g_padRaw short-circuits the default mapping.
		std::atomic_bool               g_padRaw{ false };
		// While set (overlay captures input), the thunks mark gamepad events
		// status=kStop after recording them, so they never reach the player
		// controls (see the header). Keyboard/mouse are never touched.
		std::atomic_bool               g_padConsume{ false };
		constexpr std::size_t          kPadQueueCap = 64;
		std::mutex                     g_padMutex;  // leaf lock, guards the ring below
		EngineInput::GamepadButtonEdge g_padQueue[kPadQueueCap]{};
		std::size_t                    g_padHead{ 0 };
		std::size_t                    g_padCount{ 0 };
		std::atomic<float>             g_lx{ 0.0f }, g_ly{ 0.0f }, g_rx{ 0.0f }, g_ry{ 0.0f };

		// Stick staleness guard. Unproven whether the engine sends a final
		// zero-deflection ThumbstickEvent on release or just stops dispatching;
		// if it stops, latest-wins would hold the last deflection forever and the
		// default mapping would auto-repeat until the overlay closes. GetSticks()
		// therefore reports zero once the last write is older than this.
		constexpr std::int64_t    kStickStaleMs = 150;
		std::atomic<std::int64_t> g_stickWriteMs{ 0 };

		std::int64_t NowMs()
		{
			return std::chrono::duration_cast<std::chrono::milliseconds>(
				std::chrono::steady_clock::now().time_since_epoch())
			    .count();
		}

		constexpr std::int32_t kStickLeftId = 0x0B;
		constexpr std::int32_t kStickRightId = 0x0C;

		// Receiver thunks; `this` is the BSInputEventUser subobject.

		// Accept every event type the dispatcher offers so the typed slots below
		// are exercised. This gate itself never touches event->status; the typed
		// thunks consume GAMEPAD events (status=kStop) while g_padConsume is set,
		// and keyboard/mouse pass through untouched.
		bool Thunk_ShouldHandleEvent(void*, const RE::InputEvent*)
		{
			g_shouldCalls.fetch_add(1, std::memory_order_relaxed);
			return true;
		}

		// Mark an event consumed so receivers after us in the dispatch order —
		// notably the player controls — skip it. The player's thumbstick movement
		// ignores the ControlLayer disable flags (the player walked around under
		// the open overlay), so consumption here is the only reliable gate.
		void ConsumeEvent(const void* a_event)
		{
			const_cast<RE::InputEvent*>(static_cast<const RE::InputEvent*>(a_event))->status =
				RE::InputEvent::Status::kStop;
		}

		void Thunk_OnThumbstick(void*, const void* a_event)
		{
			g_sticks.fetch_add(1, std::memory_order_relaxed);
			if (!a_event) {
				return;
			}
			// ThumbstickEvent (proven layout): IDEvent base, x/y @ +0x38/+0x3C,
			// idCode 0x0B left / 0x0C right. One event carries both axes of one
			// stick; latest-wins so the main-thread drain sees current deflection.
			const auto* b = reinterpret_cast<const std::uint8_t*>(a_event);
			const auto  id = *reinterpret_cast<const std::int32_t*>(b + 0x30);
			const float x = *reinterpret_cast<const float*>(b + 0x38);
			const float y = *reinterpret_cast<const float*>(b + 0x3C);
			if (id == kStickLeftId) {
				g_lx.store(x, std::memory_order_relaxed);
				g_ly.store(y, std::memory_order_relaxed);
			} else if (id == kStickRightId) {
				g_rx.store(x, std::memory_order_relaxed);
				g_ry.store(y, std::memory_order_relaxed);
			}
			g_stickWriteMs.store(NowMs(), std::memory_order_relaxed);
			if (g_padConsume.load(std::memory_order_relaxed)) {
				ConsumeEvent(a_event);  // recorded above; the player must not also walk with it
			}
			if (Log::DevMode()) {
				REX::DEBUG("EngineInput: stick id={:#x} x={:.3f} y={:.3f}", id, x, y);
			}
		}

		void Thunk_OnCursorMove(void*, const void*)
		{
			g_cursorMoves.fetch_add(1, std::memory_order_relaxed);
		}

		void Thunk_OnMouseMove(void*, const void*)
		{
			g_mouseMoves.fetch_add(1, std::memory_order_relaxed);
		}

		void Thunk_OnCharacter(void*, const void* a_event)
		{
			g_chars.fetch_add(1, std::memory_order_relaxed);
			if (a_event && Log::DevMode()) {
				// CharacterEvent (proven layout): codepoint dword @ +0x28.
				REX::DEBUG("EngineInput: char U+{:04X}",
					*reinterpret_cast<const std::uint32_t*>(
						reinterpret_cast<const std::uint8_t*>(a_event) + 0x28));
			}
		}

		void Thunk_OnButton(void*, const RE::ButtonEvent* a_event)
		{
			g_buttons.fetch_add(1, std::memory_order_relaxed);
			if (!a_event) {
				return;
			}
			switch (a_event->deviceType) {
			case RE::InputEvent::DeviceType::kKeyboard:
				g_buttonsKeyboard.fetch_add(1, std::memory_order_relaxed);
				break;
			case RE::InputEvent::DeviceType::kMouse:
				g_buttonsMouse.fetch_add(1, std::memory_order_relaxed);
				break;
			case RE::InputEvent::DeviceType::kGamepad:
				g_buttonsGamepad.fetch_add(1, std::memory_order_relaxed);
				break;
			default:
				break;
			}
			// Proven edge semantics: down = value!=0 && held==0; release = value==0.
			const bool down = a_event->value != 0.0f && a_event->heldDownSecs == 0.0f;
			const bool release = a_event->value == 0.0f;
			{
				std::lock_guard lock(g_ringMutex);
				g_ring[g_ringNext] = { a_event->idCode,
					static_cast<std::uint32_t>(a_event->deviceType), down };
				g_ringNext = (g_ringNext + 1) % kRingSize;
				if (g_ringCount < kRingSize) {
					++g_ringCount;
				}
			}
			// Queue gamepad edges only (press/release; skip held repeats) for the
			// main-thread router. Keyboard/mouse stay on the WndProc path.
			if (a_event->deviceType == RE::InputEvent::DeviceType::kGamepad) {
				if (down || release) {
					std::lock_guard lock(g_padMutex);
					if (g_padCount == kPadQueueCap) {  // full: drop the oldest edge
						g_padHead = (g_padHead + 1) % kPadQueueCap;
						--g_padCount;
					}
					g_padQueue[(g_padHead + g_padCount) % kPadQueueCap] = { static_cast<std::uint32_t>(a_event->idCode), down };
					++g_padCount;
				}
				// Held repeats are consumed too — a held button must not leak either.
				if (g_padConsume.load(std::memory_order_relaxed)) {
					ConsumeEvent(a_event);
				}
			}
			if (Log::DevMode()) {
				REX::DEBUG("EngineInput: button dev={} id={:#x} value={:.2f} held={:.2f}",
					static_cast<std::uint32_t>(a_event->deviceType), a_event->idCode,
					a_event->value, a_event->heldDownSecs);
			}
		}

		void BuildReceiverVtable()
		{
			if (g_recvBuilt.load(std::memory_order_acquire)) {
				return;
			}
			// RE::VTABLE::IMenu = { 475515 primary, 475519 (+0x50 event sink),
			// 475517 (+0x10 BSInputEventUser) } — index 2 is the receiver vtable
			// (array order is publication order, not subobject memory order).
			static REL::Relocation<std::uintptr_t> engineVtbl{ RE::VTABLE::IMenu[2] };
			const auto* src = reinterpret_cast<void* const*>(engineVtbl.address());
			g_recvStore[0] = src[-1];  // RTTI COL — mandatory (see header)
			for (std::size_t i = 0; i < kRecvSlots; ++i) {
				g_recvVtable[i] = src[i];
			}
			g_recvVtable[1] = reinterpret_cast<void*>(&Thunk_ShouldHandleEvent);  // 01
			g_recvVtable[4] = reinterpret_cast<void*>(&Thunk_OnThumbstick);       // 04
			g_recvVtable[5] = reinterpret_cast<void*>(&Thunk_OnCursorMove);       // 05
			g_recvVtable[6] = reinterpret_cast<void*>(&Thunk_OnMouseMove);        // 06
			g_recvVtable[7] = reinterpret_cast<void*>(&Thunk_OnCharacter);        // 07
			g_recvVtable[8] = reinterpret_cast<void*>(&Thunk_OnButton);           // 08
			// Slots 0 (dtor), 2 (kinect), 3 (deviceConnect), 9 (Unk09 held/release
			// admission) stay on engine code.
			g_recvBuilt.store(true, std::memory_order_release);
		}
	}

	void EngineInput::SetEnabled(bool a_enabled)
	{
		g_enabled.store(a_enabled, std::memory_order_release);
	}

	bool EngineInput::IsEnabled()
	{
		return g_enabled.load(std::memory_order_acquire);
	}

	void EngineInput::InstallReceiver(void* a_menuObj)
	{
		if (!a_menuObj || !IsEnabled()) {
			return;
		}
		BuildReceiverVtable();
		// The receiver subobject lives at IMenu+0x10; its vptr is the first
		// pointer there. Base-init installed the engine vtable; swap in the
		// patched copy (engine slots except the six observed ones).
		*reinterpret_cast<void**>(static_cast<std::uint8_t*>(a_menuObj) + 0x10) = &g_recvVtable[0];
		REX::INFO("EngineInput: receiver installed on menu obj=0x{:016X} (+0x10 vtable copy, observer only)",
			reinterpret_cast<std::uintptr_t>(a_menuObj));
	}

	void EngineInput::LogSessionSummary()
	{
		if (!IsEnabled()) {
			return;
		}
		const auto should = g_shouldCalls.exchange(0, std::memory_order_relaxed);
		const auto buttons = g_buttons.exchange(0, std::memory_order_relaxed);
		const auto kb = g_buttonsKeyboard.exchange(0, std::memory_order_relaxed);
		const auto mouse = g_buttonsMouse.exchange(0, std::memory_order_relaxed);
		const auto pad = g_buttonsGamepad.exchange(0, std::memory_order_relaxed);
		const auto sticks = g_sticks.exchange(0, std::memory_order_relaxed);
		const auto chars = g_chars.exchange(0, std::memory_order_relaxed);
		const auto mm = g_mouseMoves.exchange(0, std::memory_order_relaxed);
		const auto cm = g_cursorMoves.exchange(0, std::memory_order_relaxed);

		std::string recent;
		{
			std::lock_guard lock(g_ringMutex);
			for (std::size_t i = 0; i < g_ringCount; ++i) {
				const auto& r = g_ring[(g_ringNext + kRingSize - g_ringCount + i) % kRingSize];
				recent += std::format(" {}{:#x}{}",
					r.deviceType == 2 ? "pad:" : (r.deviceType == 1 ? "mouse:" : "kb:"),
					static_cast<std::uint32_t>(r.idCode), r.down ? "v" : "^");
			}
			g_ringCount = 0;
			g_ringNext = 0;
		}

		// Clear routing state so a released stick / unpopped edge can't leak into
		// the next overlay session.
		{
			std::lock_guard lock(g_padMutex);
			g_padHead = 0;
			g_padCount = 0;
		}
		g_lx.store(0.0f, std::memory_order_relaxed);
		g_ly.store(0.0f, std::memory_order_relaxed);
		g_rx.store(0.0f, std::memory_order_relaxed);
		g_ry.store(0.0f, std::memory_order_relaxed);
		g_stickWriteMs.store(0, std::memory_order_relaxed);

		if (should == 0 && buttons == 0 && sticks == 0 && chars == 0 && mm == 0 && cm == 0) {
			if (Log::DevMode()) {
				REX::INFO("EngineInput: session summary — no engine dispatch observed (menu likely never admitted or no input while open)");
			}
			return;
		}
		if (Log::DevMode()) {
			REX::INFO(
				"EngineInput: session summary — gates={} buttons={} (kb={} mouse={} pad={}) sticks={} chars={} mouseMoves={} cursorMoves={}; recent buttons:{}",
				should, buttons, kb, mouse, pad, sticks, chars, mm, cm,
				recent.empty() ? " none" : recent.c_str());
		}
	}

	bool EngineInput::PollGamepadButton(GamepadButtonEdge& a_out)
	{
		std::lock_guard lock(g_padMutex);
		if (g_padCount == 0) {
			return false;
		}
		a_out = g_padQueue[g_padHead];
		g_padHead = (g_padHead + 1) % kPadQueueCap;
		--g_padCount;
		return true;
	}

	EngineInput::GamepadSticks EngineInput::GetSticks()
	{
		// No fresh thumbstick dispatch within kStickStaleMs means "centered", so a
		// possibly-missing release event cannot leave the mapping auto-repeating.
		if (NowMs() - g_stickWriteMs.load(std::memory_order_relaxed) > kStickStaleMs) {
			return {};
		}
		return {
			g_lx.load(std::memory_order_relaxed), g_ly.load(std::memory_order_relaxed),
			g_rx.load(std::memory_order_relaxed), g_ry.load(std::memory_order_relaxed)
		};
	}

	void EngineInput::SetRawMode(bool a_raw)
	{
		if (g_padRaw.exchange(a_raw, std::memory_order_relaxed) != a_raw) {
			REX::INFO("EngineInput: gamepad raw-passthrough mode {} (default nav/scroll mapping {})",
				a_raw ? "ON" : "off", a_raw ? "suppressed" : "active");
		}
	}

	void EngineInput::SetConsumeGamepad(bool a_consume)
	{
		if (g_padConsume.exchange(a_consume, std::memory_order_relaxed) != a_consume) {
			REX::INFO("EngineInput: gamepad consume-at-receiver {} (engine {} sees pad input past the overlay)",
				a_consume ? "ON" : "off", a_consume ? "no longer" : "again");
		}
	}

}
