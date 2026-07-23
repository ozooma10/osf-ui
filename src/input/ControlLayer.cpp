#include "input/ControlLayer.h"

#include "RE/B/BSInputEnableLayer.h"
#include "RE/B/BSInputEnableManager.h"
#include "RE/U/UserEvents.h"

#include "core/Log.h"
#include "core/MainThreadLatch.h"

namespace OSFUI
{
	namespace
	{
		// BSInputEnableManager/BSInputEnableLayer are main-thread-owned, but
		// Runtime::Tick runs on an off-main worker (proven 2026-07-23), so the
		// engage/release is marshalled onto the main thread by this latch. g_layer
		// is therefore only ever touched on the main thread (inside ApplyOnMain).
		MainThreadLatch         g_latch;
		RE::BSInputEnableLayer* g_layer{ nullptr };

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

	namespace
	{
		// Runs on the game MAIN thread (via g_latch). Returns false to defer when
		// the input manager isn't ready yet (main menu), so Request retries next
		// tick. The session layer is held and its mask toggled rather than
		// DecRef-on-close: DecRef does work (engine release runs LayerFreed and
		// restores controls), but holding it avoids re-claiming a pool slot on
		// every overlay toggle.
		bool ApplyOnMain(bool a_engage)
		{
			if (a_engage) {
				if (!EnsureLayer()) {
					return false;  // not in gameplay yet; retry next tick
				}
				g_layer->EnableUserEvent(kUserDisable, false);
				g_layer->EnableOtherEvent(kOtherDisable, false);
				REX::DEBUG("ControlLayer: player controls disabled (layer {})", g_layer->GetLayerID());
			} else {
				if (g_layer) {
					// Re-enable exactly what we disabled.
					g_layer->EnableUserEvent(kUserDisable, true);
					g_layer->EnableOtherEvent(kOtherDisable, true);
				}
				REX::DEBUG("ControlLayer: player controls restored");
			}
			return true;
		}
	}

	void ControlLayer::Apply(bool a_engage)
	{
		g_latch.Request(a_engage, &ApplyOnMain);
	}
}
