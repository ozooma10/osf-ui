#include "runtime/SettingsModule.h"

#include "runtime/Json.h"
#include "runtime/MessageBridge.h"

namespace SWUI
{
	SettingsModule::SettingsModule(std::filesystem::path a_schemaDir,
		std::filesystem::path a_valuesDir,
		SettingsStore::ChangeListener a_onChange) :
		_schemaDir(std::move(a_schemaDir)),
		_valuesDir(std::move(a_valuesDir))
	{
		_store.SetChangeListener(std::move(a_onChange));
		_store.LoadAll(_schemaDir, _valuesDir);
	}

	void SettingsModule::OnStart()
	{
		// Push persisted values through the change listener so reactions (e.g.
		// cursor speed) apply before the first frame.
		_store.NotifyAll();
	}

	void SettingsModule::RegisterCommands(MessageBridge& a_bridge)
	{
		a_bridge.RegisterCommand("settings.get", [this](const nlohmann::json&, MessageBridge& a_b) {
			a_b.SendToWeb("settings.data", nlohmann::json::parse(_store.DataJson(), nullptr, false));
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
			if (_store.Reset(mod, key)) {
				// Re-send the registry so the view re-renders to the new state.
				a_b.SendToWeb("settings.data", nlohmann::json::parse(_store.DataJson(), nullptr, false));
			}
		});
	}
}
