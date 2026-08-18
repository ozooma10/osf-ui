#include "Input/EngineInput.h"

#include "RE/B/BSInputEventUser.h"
#include "RE/IDs_VTABLE.h"

#include "Core/Log.h"

#include <atomic>
#include <cstdint>

namespace OSFUI
{
	namespace
	{
		// Patched +0x10 receiver vtable (copy of engine 475517): 10 slots
		// (0 dtor .. 9 Unk09) plus one leading slot for the engine's RTTI COL
		// (vtable[-1]), without which dynamic_cast through the copy fails — the
		// failure mode the primary-vtable copy hit on Route A.
		constexpr std::size_t kRecvSlots = 10;
		std::atomic_bool      g_recvBuilt{ false };
		void*                 g_recvStore[kRecvSlots + 1]{};
		void** const          g_recvVtable = &g_recvStore[1];

		// Runtime owns the policy on the main thread. Engine input dispatch reads it from worker threads, so this is the gate's only shared state.
		std::atomic_bool g_gamepadCapture{ false };

		// Receiver thunks; `this` is the BSInputEventUser subobject.

		// Accept every event type the dispatcher offers so the typed slots below
		// are exercised. This gate itself never touches event->status; the typed
		// thunks consume GAMEPAD events (status=kStop) while capture is set,
		// and keyboard/mouse pass through untouched.
		bool Thunk_ShouldHandleEvent(void*, const RE::InputEvent*)
		{
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
			if (a_event && g_gamepadCapture.load(std::memory_order_relaxed)) {
				ConsumeEvent(a_event);
			}
		}

		// Cursor/mouse-move slots exist only so the receiver vtable owns them;
		// keyboard/mouse stay on the WndProc path, so nothing to route here.
		void Thunk_OnCursorMove(void*, const void*) {}
		void Thunk_OnMouseMove(void*, const void*) {}

		void Thunk_OnCharacter(void*, const void* a_event)
		{
			if (a_event && Log::DebugEnabled()) {
				// CharacterEvent (proven layout): codepoint dword @ +0x28.
				REX::DEBUG("EngineInput: char U+{:04X}",
					*reinterpret_cast<const std::uint32_t*>(
						reinterpret_cast<const std::uint8_t*>(a_event) + 0x28));
			}
		}

		void Thunk_OnButton(void*, const RE::ButtonEvent* a_event)
		{
			if (!a_event) {
				return;
			}
			if (a_event->deviceType == RE::InputEvent::DeviceType::kGamepad) {
				if (g_gamepadCapture.load(std::memory_order_relaxed)) {
					ConsumeEvent(a_event);
				}
			}
			if (Log::DebugEnabled()) {
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

	void EngineInput::InstallReceiver(void* a_menuObj)
	{
		if (!a_menuObj) {
			return;
		}
		BuildReceiverVtable();
		// The receiver subobject lives at IMenu+0x10; its vptr is the first
		// pointer there. Base-init installed the engine vtable; swap in the
		// patched copy (engine slots except the six observed ones).
		*reinterpret_cast<void**>(static_cast<std::uint8_t*>(a_menuObj) + 0x10) = &g_recvVtable[0];
		REX::DEBUG("EngineInput: gamepad capture gate installed on menu obj=0x{:016X} (+0x10 vtable copy)", reinterpret_cast<std::uintptr_t>(a_menuObj));
	}

	void EngineInput::SetGamepadCapture(bool a_capture)
	{
		if (g_gamepadCapture.exchange(a_capture, std::memory_order_relaxed) != a_capture) {
			REX::DEBUG("EngineInput: gamepad capture gate {} (Starfield {} controller input)", a_capture ? "ON" : "off", a_capture ? "no longer receives" : "receives");
		}
	}

}
