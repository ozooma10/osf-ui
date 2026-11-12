#include "Input/ControlLayer.h"

#include "RE/B/BSInputEnableLayer.h"
#include "RE/B/BSInputEnableManager.h"
#include "RE/U/UserEvents.h"

#include "Core/Log.h"

namespace OSFUI
{
	namespace
	{
		// Save completion can arrive on a worker while the runtime changes capture.
		std::mutex              g_lock;
		RE::BSInputEnableLayer* g_layer{ nullptr };
		bool                    g_engaged{ false };
		bool                    g_requested{ false };
		bool                    g_saving{ false };
		bool                    g_loading{ false };
		unsigned                g_requestedSaveDepth{ 0 };

		// Masks proven on 1.16.244. Menu stays enabled.
		constexpr RE::USER_EVENT_FLAG kUserDisable =
			RE::USER_EVENT_FLAG::Movement |     // Walking | Jumping
			RE::USER_EVENT_FLAG::Looking |
			RE::USER_EVENT_FLAG::Fighting |
			RE::USER_EVENT_FLAG::Sneaking |
			RE::USER_EVENT_FLAG::Activation |
			RE::USER_EVENT_FLAG::POVSwitch |
			RE::USER_EVENT_FLAG::WheelZoom;

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

		void Reconcile()
		{
			const bool engage = g_requested && !g_saving && !g_loading && !g_requestedSaveDepth;
			if (engage == g_engaged) return;
			if (engage) {
				if (!EnsureLayer()) return;  // retry next tick
				g_layer->EnableUserEvent(kUserDisable, false);
				g_layer->EnableOtherEvent(kOtherDisable, false);
				REX::DEBUG("ControlLayer: player controls disabled (layer {})", g_layer->GetLayerID());
			} else {
				if (g_layer) {
					g_layer->EnableUserEvent(kUserDisable, true);
					g_layer->EnableOtherEvent(kOtherDisable, true);
				}
				REX::DEBUG("ControlLayer: player controls restored");
			}
			g_engaged = engage;
		}
	}

	void ControlLayer::Apply(bool a_engage)
	{
		std::scoped_lock lock(g_lock);
		g_requested = a_engage;
		Reconcile();
	}

	void ControlLayer::SaveWindow(bool a_saving)
	{
		std::scoped_lock lock(g_lock);
		g_saving = a_saving;
		Reconcile();
	}

	void ControlLayer::BeginRequestedSave()
	{
		std::scoped_lock lock(g_lock);
		++g_requestedSaveDepth;
		Reconcile();
	}

	void ControlLayer::EndRequestedSave()
	{
		std::scoped_lock lock(g_lock);
		if (g_requestedSaveDepth) --g_requestedSaveDepth;
		Reconcile();
	}

	void ControlLayer::BeginLoad()
	{
		std::scoped_lock lock(g_lock);
		g_requested = false;
		g_saving = false;
		g_loading = true;
		Reconcile();
	}

	void ControlLayer::EndLoad()
	{
		std::scoped_lock lock(g_lock);
		g_requested = false;
		g_saving = false;
		g_loading = false;
		// A loaded save may restore this layer's old mask even if the previous
		// world closed its panel. Reconcile only our layer, never other owners.
		g_engaged = g_layer != nullptr;
		Reconcile();
	}
}
