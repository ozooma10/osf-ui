#include "Input/ControlLayer.h"

#include "RE/B/BSInputEnableLayer.h"
#include "RE/B/BSInputEnableManager.h"
#include "RE/U/UserEvents.h"

#include "Core/Log.h"

namespace OSFUI
{
	namespace
	{
		// Main-thread-only state (Apply runs from Runtime::Tick).
		RE::BSInputEnableLayer* g_layer{ nullptr };
		bool                    g_engaged{ false };

		// Keep Menu enabled while disabling the 1.16.244-proven player-control masks.
		constexpr RE::USER_EVENT_FLAG kUserDisable =
			RE::USER_EVENT_FLAG::Movement |     // Walking | Jumping
			RE::USER_EVENT_FLAG::Looking |      // mouse-look / camera (proven)
			RE::USER_EVENT_FLAG::Fighting |
			RE::USER_EVENT_FLAG::Sneaking |
			RE::USER_EVENT_FLAG::Activation |   // activate / use (proven)
			RE::USER_EVENT_FLAG::POVSwitch |    // 1st/3rd-person toggle (proven)
			RE::USER_EVENT_FLAG::WheelZoom;     // zoom (proven)

		constexpr RE::OTHER_EVENT_FLAG kOtherDisable =
			RE::OTHER_EVENT_FLAG::Activate |
			RE::OTHER_EVENT_FLAG::VATS |
			RE::OTHER_EVENT_FLAG::Favorites |
			RE::OTHER_EVENT_FLAG::Running |
			RE::OTHER_EVENT_FLAG::Sprinting |
			RE::OTHER_EVENT_FLAG::FastTravel |
			RE::OTHER_EVENT_FLAG::GravJump |
			RE::OTHER_EVENT_FLAG::Takeoff |
			// Disable gamepad-reachable verbs that bypass the window hook.
			RE::OTHER_EVENT_FLAG::HandScanner |  // LB
			RE::OTHER_EVENT_FLAG::Journal |      // Start
			RE::OTHER_EVENT_FLAG::Inventory |
			RE::OTHER_EVENT_FLAG::FarTravel;

		// Allocate on first use and retry next tick while the manager is unavailable.
		bool EnsureLayer()
		{
			if (g_layer) {
				return true;
			}
			auto* manager = RE::BSInputEnableManager::GetSingleton();
			if (!manager) {
				static std::once_flag once;
				Log::WarnOnce(once, "ControlLayer: BSInputEnableManager not ready (main menu?); "
									"control-disable deferred until gameplay");
				return false;
			}
			if (!manager->AllocateNewLayer(&g_layer, "OSF UI Overlay") || !g_layer) {
				REX::ERROR("ControlLayer: AllocateNewLayer failed; control-disable unavailable");
				g_layer = nullptr;
				return false;
			}
			REX::DEBUG("ControlLayer: allocated input-enable layer (id {})", g_layer->GetLayerID());
			return true;
		}
	}

	void ControlLayer::Apply(bool a_engage)
	{
		if (a_engage == g_engaged) {
			return;
		}
		if (a_engage) {
			if (!EnsureLayer()) {
				return;  // not in gameplay yet; retry next tick
			}
			g_layer->EnableUserEvent(kUserDisable, false);
			g_layer->EnableOtherEvent(kOtherDisable, false);
			REX::DEBUG("ControlLayer: player controls disabled (layer {})", g_layer->GetLayerID());
		} else {
			if (g_layer) {
				// Restore exactly this retained layer's disabled masks.
				g_layer->EnableUserEvent(kUserDisable, true);
				g_layer->EnableOtherEvent(kOtherDisable, true);
			}
			REX::DEBUG("ControlLayer: player controls restored");
		}
		g_engaged = a_engage;
	}
}
