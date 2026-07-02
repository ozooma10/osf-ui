#include "input/EngineInput.h"

#include "RE/B/BSInputEventUser.h"
#include "RE/IDs_VTABLE.h"

#include "core/Log.h"

#include <atomic>
#include <cstdint>
#include <format>
#include <mutex>
#include <string>

namespace OSFUI
{
	namespace
	{
		std::atomic_bool g_enabled{ false };

		// ---- patched +0x10 receiver vtable (copy of engine 475517) ----
		// 10 slots (0 dtor .. 9 Unk09) + one LEADING slot for the engine's RTTI
		// COL (vtable[-1]) so dynamic_cast through the copy stays valid — the
		// exact failure mode the primary-vtable copy hit on Route A.
		constexpr std::size_t kRecvSlots = 10;
		std::atomic_bool      g_recvBuilt{ false };
		void*                 g_recvStore[kRecvSlots + 1]{};
		void** const          g_recvVtable = &g_recvStore[1];

		// ---- observation state (thunks run on the engine's worker pool) ----
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

		// ---- receiver thunks (this = the BSInputEventUser subobject) ----

		// Accept every event type the dispatcher offers so the typed slots below
		// are exercised. Observation only: we never touch event->status, so
		// downstream menus see exactly what they saw before.
		bool Thunk_ShouldHandleEvent(void*, const RE::InputEvent*)
		{
			g_shouldCalls.fetch_add(1, std::memory_order_relaxed);
			return true;
		}

		void Thunk_OnThumbstick(void*, const void* a_event)
		{
			g_sticks.fetch_add(1, std::memory_order_relaxed);
			if (a_event && Log::DevMode()) {
				// ThumbstickEvent (proven layout): IDEvent base, x/y @ +0x38/+0x3C,
				// idCode 0x0B left / 0x0C right.
				const auto* b = reinterpret_cast<const std::uint8_t*>(a_event);
				REX::DEBUG("EngineInput: stick id={:#x} x={:.3f} y={:.3f}",
					*reinterpret_cast<const std::int32_t*>(b + 0x30),
					*reinterpret_cast<const float*>(b + 0x38),
					*reinterpret_cast<const float*>(b + 0x3C));
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
			{
				std::lock_guard lock(g_ringMutex);
				g_ring[g_ringNext] = { a_event->idCode,
					static_cast<std::uint32_t>(a_event->deviceType), down };
				g_ringNext = (g_ringNext + 1) % kRingSize;
				if (g_ringCount < kRingSize) {
					++g_ringCount;
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
			// (array order is publication order, NOT subobject memory order).
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
		// pointer there. base-init installed the engine vtable; replace it with
		// our patched copy (engine slots except the six we observe).
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

		if (should == 0 && buttons == 0 && sticks == 0 && chars == 0 && mm == 0 && cm == 0) {
			REX::INFO("EngineInput: session summary — no engine dispatch observed (menu likely never admitted or no input while open)");
			return;
		}
		REX::INFO(
			"EngineInput: session summary — gates={} buttons={} (kb={} mouse={} pad={}) sticks={} chars={} mouseMoves={} cursorMoves={}; recent buttons:{}",
			should, buttons, kb, mouse, pad, sticks, chars, mm, cm,
			recent.empty() ? " none" : recent.c_str());
	}
}
