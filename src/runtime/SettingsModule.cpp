#include "runtime/SettingsModule.h"

#include "runtime/Json.h"
#include "runtime/MessageBridge.h"

namespace OSFUI
{
	SettingsModule::SettingsModule(std::filesystem::path a_schemaDir,
		std::filesystem::path a_valuesDir,
		SettingsStore::ChangeListener a_onChange) :
		_schemaDir(std::move(a_schemaDir)),
		_valuesDir(std::move(a_valuesDir))
	{
		// Subscriber #0: the runtime's core reaction (framework knobs). Later
		// listeners (web push, ABI mirror) multicast behind it.
		_store.AddChangeListener(std::move(a_onChange));
		// Subscriber #1: the web push — every committed value goes to every
		// view that has read the registry (mcm-design.md §8.5). The no-listener
		// guard runs BEFORE the payload is built: startup NotifyAll and every
		// set with no view open would otherwise allocate json for nobody.
		_store.AddChangeListener([this](std::string_view a_mod, std::string_view a_key, const nlohmann::json& a_value) {
			if (_suppressChangedPush || !_bridge || _subscribers.empty()) {
				return;
			}
			PushToSubscribers("settings.changed", {
				{ "mod", std::string(a_mod) },
				{ "key", std::string(a_key) },
				{ "value", a_value },
			});
		});
		// Registry shape changed (runtime registration/removal while views are
		// live): re-send the full document — the settings view fully re-renders
		// on settings.data, so late registration Just Works.
		_store.AddRegistryListener([this] {
			if (!_bridge || _subscribers.empty()) {
				return;
			}
			PushToSubscribers("settings.data", _store.Data());
		});
		_store.LoadAll(_schemaDir, _valuesDir);
	}

	void SettingsModule::OnStart()
	{
		// Push persisted values through the change listener so reactions (e.g.
		// cursor speed) apply before the first frame.
		_store.NotifyAll();
	}

	void SettingsModule::OnBridgeDown()
	{
		_bridge = nullptr;
		_subscribers.clear();
	}

	void SettingsModule::OnViewDestroyed(std::string_view a_viewId)
	{
		// Mirror of the runtime's _viewsSubscribers pruning: a view torn down by
		// crash-recovery must not keep receiving pushes for the process lifetime.
		_subscribers.erase(std::string(a_viewId));
	}

	void SettingsModule::PushToSubscribers(std::string_view a_type, const nlohmann::json& a_payload) const
	{
		if (!_bridge || _subscribers.empty()) {
			return;
		}
		for (const auto& id : _subscribers) {
			_bridge->SendToWeb(id, a_type, a_payload);
		}
	}

	void SettingsModule::RegisterCommands(MessageBridge& a_bridge)
	{
		// A new bridge means the view layer was (re)built: pages reload and
		// re-subscribe via settings.get, so drop the stale subscriber set.
		_bridge = &a_bridge;
		_subscribers.clear();

		a_bridge.RegisterCommand("settings.get", [this](const nlohmann::json&, MessageBridge& a_b) {
			// Subscribe-on-read (the views.get pattern): the caller now also
			// receives settings.changed pushes and settings.data re-broadcasts.
			_subscribers.insert(std::string(a_b.CurrentSource()));
			a_b.SendToWeb("settings.data", _store.Data());
		});

		a_bridge.RegisterCommand("settings.set", [this](const nlohmann::json& a_payload, MessageBridge& a_b) {
			const auto mod = Json::GetString(a_payload, "mod", "");
			const auto key = Json::GetString(a_payload, "key", "");
			const auto valueIt = a_payload.find("value");
			const bool ok = valueIt != a_payload.end() && _store.Set(mod, key, valueIt->dump());
			a_b.SendToWeb("settings.ack", { { "mod", mod }, { "key", key }, { "ok", ok } });
		});

		a_bridge.RegisterCommand("settings.reset", [this](const nlohmann::json& a_payload, MessageBridge& a_b) {
			const auto mod = Json::GetString(a_payload, "mod", "");
			const auto key = Json::GetString(a_payload, "key", "");
			// Suppress the per-key settings.changed fan-out for the web: the one
			// authoritative settings.data below syncs every subscriber (a whole-mod
			// reset would otherwise send N redundant messages first). The core
			// change listener (native reactions) still fires per key.
			_suppressChangedPush = true;
			const bool ok = _store.Reset(mod, key);
			_suppressChangedPush = false;
			if (ok) {
				const auto data = _store.Data();
				PushToSubscribers("settings.data", data);
				if (!_subscribers.contains(std::string(a_b.CurrentSource()))) {
					// A caller that never subscribed still needs to re-render.
					a_b.SendToWeb("settings.data", data);
				}
			}
		});
	}
}
