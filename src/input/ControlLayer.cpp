#include "input/ControlLayer.h"

#include "RE/B/BSInputEnableLayer.h"
#include "RE/B/BSInputEnableManager.h"
#include "RE/U/UserEvents.h"

#include "core/Log.h"

namespace OSFUI
{
	namespace
	{
		// Main-thread-only state (Engage/Release run from Runtime::Tick).
		RE::BSInputEnableLayer* g_layer{ nullptr };
		bool                    g_engaged{ false };

		// Flags cleared while the overlay is open, to freeze the player. `Menu`
		// stays enabled so the engine cursor/menu path keeps working (notably
		// alongside FocusMenu). These bits are runtime-proven on 1.16.244:
		// Looking(b1) is the mouse-look/camera bit, so OTHER::CamSwitch is not
		// needed for camera and is absent below. TabMenuMaybe/Console stayed out
		// (inconclusive / disproven).
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
			// Gamepad-reachable verbs that leaked with the overlay open
			// (2026-07-02: LB opened the hand scanner under the menu):
			RE::OTHER_EVENT_FLAG::HandScanner |  // LB
			RE::OTHER_EVENT_FLAG::Journal |      // Start
			RE::OTHER_EVENT_FLAG::Inventory |
			RE::OTHER_EVENT_FLAG::FarTravel;

		// Allocates the session layer on first use. Returns false when the manager
		// isn't ready yet (main menu); the caller retries next tick.
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

	void ControlLayer::Engage()
	{
		if (g_engaged) {
			return;
		}
		if (!EnsureLayer()) {
			return;  // not in gameplay yet; caller retries
		}
		g_layer->EnableUserEvent(kUserDisable, false);
		g_layer->EnableOtherEvent(kOtherDisable, false);
		g_engaged = true;
		REX::DEBUG("ControlLayer: player controls disabled (layer {})", g_layer->GetLayerID());
	}

	void ControlLayer::Release()
	{
		if (!g_engaged) {
			return;
		}
		if (g_layer) {
			// Re-enable exactly what we disabled. The layer is held for the session
			// and its mask toggled rather than DecRef-on-close: DecRef does work
			// (engine release runs LayerFreed and restores controls), but holding it
			// avoids re-claiming a pool slot on every overlay toggle.
			g_layer->EnableUserEvent(kUserDisable, true);
			g_layer->EnableOtherEvent(kOtherDisable, true);
		}
		g_engaged = false;
		REX::DEBUG("ControlLayer: player controls restored");
	}

	bool ControlLayer::IsEngaged()
	{
		return g_engaged;
	}
}
