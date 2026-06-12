#include "input/UiInputHook.h"

#include "RE/B/BSInputEventUser.h"
#include "RE/U/UI.h"
#include "SFSE/InputMap.h"

#include "runtime/Runtime.h"

namespace SWUI
{
	namespace
	{
		using PerformInputProcessing_t = void(RE::UI*, const RE::InputEvent*);

		std::atomic_bool             g_enabled{ false };
		bool                         g_installed{ false };
		PerformInputProcessing_t*    g_original{ nullptr };

		void ObserveButtonEvent(const RE::ButtonEvent* a_event)
		{
			// Keyboard idCodes are already in InputMap space (0..255, DIK
			// scan codes); mouse buttons get the InputMap offset so the whole
			// pipeline speaks one key space.
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
					if (a_event->idCode <= 2 && a_event->heldDownSecs == 0.0f) {
						input.OnMouseButton(static_cast<MouseButton>(a_event->idCode), a_event->value != 0.0f);
					}
					break;
				}
				default:
					// Gamepad routing needs a focus model first (Phase 4).
					break;
			}
		}

		void Thunk(RE::UI* a_this, const RE::InputEvent* a_queueHead)
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

	bool UiInputHook::Install()
	{
		if (g_installed) {
			return true;
		}
		if (!RE::UI::GetSingleton()) {
			REX::ERROR("UiInputHook: RE::UI singleton is null; input observation unavailable");
			return false;
		}

		// RE::UI::VTABLE[0] is the BSInputEventReceiver base vtable; slot 1
		// is PerformInputProcessing (0 is the virtual destructor).
		REL::Relocation<std::uintptr_t> vtbl{ RE::UI::VTABLE[0] };
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
