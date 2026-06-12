#include "input/UiInputHook.h"

#include "RE/B/BSInputEventReceiver.h"
#include "RE/B/BSInputEventUser.h"
#include "RE/U/UI.h"
#include "SFSE/InputMap.h"

#include "runtime/Runtime.h"

namespace SWUI
{
	namespace
	{
		// `this` for PerformInputProcessing is the BSInputEventReceiver
		// subobject (UI + 0x10), not the UI base.
		using PerformInputProcessing_t = void(RE::BSInputEventReceiver*, const RE::InputEvent*);

		std::atomic_bool             g_enabled{ false };
		bool                         g_installed{ false };
		PerformInputProcessing_t*    g_original{ nullptr };

		// Index of the BSInputEventReceiver vtable in RE::UI::VTABLE
		// (AddressLib ID 475439). Proven on game 1.16.244 — see
		// VerifyUiLayout(), which hard-fails if this stops being true.
		constexpr std::size_t kReceiverVtblIndex = 10;

		void ObserveButtonEvent(const RE::ButtonEvent* a_event)
		{
			// Keyboard idCodes are Windows VK codes (in-game proof in
			// InputTypes.h); the router speaks the same space.
			auto& input = Runtime::Get().Input();

			switch (a_event->deviceType) {
				case RE::InputEvent::DeviceType::kKeyboard:
				{
					const auto code = static_cast<KeyCode>(a_event->idCode);
					if (a_event->value != 0.0f) {
						// Initial press only; held repeats have heldDownSecs > 0.
						if (a_event->heldDownSecs == 0.0f) {
							input.OnKeyDown(code);
						}
					} else {
						input.OnKeyUp(code);
					}
					break;
				}
				case RE::InputEvent::DeviceType::kMouse:
				{
					if (a_event->idCode <= 2) {
						const bool pressed = a_event->value != 0.0f;
						// Initial press only — but releases always have
						// heldDownSecs > 0, so they must not be filtered
						// (verified in-game: the old filter ate every
						// mouse-up).
						if (!pressed || a_event->heldDownSecs == 0.0f) {
							input.OnMouseButton(static_cast<MouseButton>(a_event->idCode), pressed);
						}
					}
					break;
				}
				default:
					// Gamepad routing needs a focus model first (Phase 4).
					break;
			}
		}

		void Thunk(RE::BSInputEventReceiver* a_this, const RE::InputEvent* a_queueHead)
		{
			if (g_enabled.load(std::memory_order_relaxed)) {
				for (const auto* event = a_queueHead; event; event = event->next) {
					if (event->eventType == RE::InputEvent::EventType::kButton) {
						ObserveButtonEvent(static_cast<const RE::ButtonEvent*>(event));
					}
				}
			}
			// ALWAYS forward the unmodified queue. This hook observes; it
			// must never change what the game sees.
			g_original(a_this, a_queueHead);
		}
	}

	bool UiInputHook::VerifyUiLayout()
	{
		auto* ui = RE::UI::GetSingleton();
		if (!ui) {
			REX::ERROR("UiInputHook: RE::UI singleton is null; layout unverifiable");
			return false;
		}

		// Cross-check the compiled UI base offsets against the running binary
		// before anything writes to or registers on the UI object: the vptr of
		// the live BSInputEventReceiver subobject must be exactly the vtable
		// AddressLib reports for it. A stale CommonLibSF layout fails this
		// instead of corrupting UI state — that exact failure shipped against
		// game 1.16.244 with a pre-PR#26 submodule and crashed on save load.
		//
		// The receiver's entry is VTABLE[kReceiverVtblIndex], NOT VTABLE[0] or
		// [1]: the IDs_VTABLE.h array order does not follow base-declaration
		// order. Proven 2026-06-12 by resolving all 11 entries from
		// versionlib-1-16-244 (tools/parse_versionlib.py) against the vptr
		// observed in the running game; the matched vtable is also the only
		// 2-slot one in the cluster (dtor + PerformInputProcessing), which is
		// exactly BSInputEventReceiver's shape. Both checks below stay hard
		// requirements: if CommonLibSF reorders the array or a patch moves the
		// vtable, this refuses and dumps the data needed to re-derive.
		auto* receiver = static_cast<RE::BSInputEventReceiver*>(ui);
		const auto liveVptr = *reinterpret_cast<const std::uintptr_t*>(receiver);
		const REL::Relocation<std::uintptr_t> vtbl{ RE::UI::VTABLE[kReceiverVtblIndex] };
		if (liveVptr != vtbl.address()) {
			REX::ERROR(
				"UiInputHook: UI layout guard FAILED — live BSInputEventReceiver vptr {:#x} != AddressLib UI::VTABLE[{}] {:#x} "
				"(CommonLibSF layout or address library stale for this game version); dumping all entries:",
				liveVptr, kReceiverVtblIndex, vtbl.address());
			for (std::size_t i = 0; i < RE::UI::VTABLE.size(); ++i) {
				const REL::Relocation<std::uintptr_t> entry{ RE::UI::VTABLE[i] };
				REX::ERROR("UiInputHook:   UI::VTABLE[{:2}] (ID {}) = {:#x}{}",
					i, RE::UI::VTABLE[i].id(), entry.address(),
					entry.address() == liveVptr ? "  <-- matches live vptr" : "");
			}
			return false;
		}
		return true;
	}

	bool UiInputHook::Install()
	{
		if (g_installed) {
			return true;
		}
		if (!VerifyUiLayout()) {
			REX::ERROR("UiInputHook: refusing to install (layout guard failed)");
			return false;
		}

		// Slot 1 is PerformInputProcessing (0 is the virtual destructor);
		// the vtable itself is live-verified by VerifyUiLayout() above.
		REL::Relocation<std::uintptr_t> vtbl{ RE::UI::VTABLE[kReceiverVtblIndex] };
		const auto                      original = vtbl.write_vfunc(1, &Thunk);
		g_original = reinterpret_cast<PerformInputProcessing_t*>(original);
		g_installed = true;
		REX::INFO("UiInputHook: installed observe-only vfunc hook on UI::PerformInputProcessing");
		return true;
	}

	void UiInputHook::SetEnabled(bool a_enabled)
	{
		g_enabled.store(a_enabled, std::memory_order_relaxed);
	}
}
