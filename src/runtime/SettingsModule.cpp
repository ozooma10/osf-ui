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
		// listeners multicast behind it.
		_store.AddChangeListener(std::move(a_onChange));
		// Subscriber #1: web push — every committed value goes to every view
		// that has read the registry (mcm-design.md §8.5). The no-listener guard
		// runs before the payload is built; startup NotifyAll and every set with
		// no view open would otherwise allocate json for nobody.
		_store.AddChangeListener([this](std::string_view a_mod, std::string_view a_key, const nlohmann::json& a_value) {
			if (_suppressChangedPush || !_bridge || _subscribers.empty()) {
				return;
			}
			nlohmann::json payload = {
				{ "mod", std::string(a_mod) },
				{ "key", std::string(a_key) },
				{ "value", a_value },
			};
			// Key-typed changes carry the setting's recomputed conflict list
			// (api-freeze-plan item 11) — always present for keys, [] = none —
			// so views update badges in place instead of re-fetching the whole
			// registry after every rebind.
			if (_store.GetSettingType(a_mod, a_key) == "key") {
				payload["conflicts"] = _store.ConflictsForSetting(a_mod, a_key);
			}
			PushToSubscribers("settings.changed", payload);
		});
		// Registry shape changed (runtime registration/removal while views are
		// live): re-send the full document. The settings view re-renders fully
		// on settings.data, so late registration needs nothing else.
		_store.AddRegistryListener([this] {
			if (!_bridge || _subscribers.empty()) {
				return;
			}
			PushToSubscribers("settings.data", _store.DataView());
		});
		// A mod's values-file write landed (the write-behind flush, distinct
		// from the immediate settings.changed commit): lets the settings UI
		// show "Saved" feedback.
		_store.AddPersistListener([this](std::string_view a_mod) {
			if (!_bridge || _subscribers.empty()) {
				return;
			}
			PushToSubscribers("settings.persisted", { { "mod", std::string(a_mod) } });
		});
		_store.LoadAll(_schemaDir, _valuesDir);
		// Seed the hot-reload snapshot from what LoadAll just consumed, so the
		// first PumpSchemaHotReload pass reloads nothing.
		_schemaMtimes = ScanSchemaDir();
	}

	SettingsModule::SchemaMtimes SettingsModule::ScanSchemaDir() const
	{
		SchemaMtimes seen;
		std::error_code ec;
		for (const auto& entry : std::filesystem::directory_iterator(_schemaDir, ec)) {
			if (entry.is_regular_file(ec) && entry.path().extension() == ".json") {
				if (const auto t = entry.last_write_time(ec); !ec) {
					seen.emplace(entry.path().stem().string(), t);
				}
			}
		}
		return seen;
	}

	void SettingsModule::PumpSchemaHotReload(double a_nowSeconds)
	{
		if (a_nowSeconds < _nextSchemaScan) {
			return;
		}
		_nextSchemaScan = a_nowSeconds + kHotReloadScanSeconds;

		auto seen = ScanSchemaDir();
		// Changed or new files reload through the store; every consequence
		// (value preservation via flush-then-overlay, §11 alias adoption,
		// settings.data re-broadcast, HotkeyService rebuild via the registry
		// listener) rides the same wiring as a runtime registration. The mtime
		// is recorded even when the reload fails (mid-save torn file, invalid
		// schema): the editor's final write bumps it again, and a broken file
		// logs once per save instead of once per scan.
		for (const auto& [stem, mtime] : seen) {
			const auto it = _schemaMtimes.find(stem);
			if (it == _schemaMtimes.end() || it->second != mtime) {
				_store.ReloadDropInFile(_schemaDir / (stem + ".json"));
			}
		}
		// A deleted file removes its mod, but only a drop-in one: a runtime
		// registration owns its schema regardless of any same-id file coming or
		// going (same precedence as load). Values files are kept (§10).
		for (const auto& [stem, mtime] : _schemaMtimes) {
			if (!seen.contains(stem) && _store.GetSource(stem) == SettingsStore::Source::kDropIn) {
				REX::INFO("SettingsModule: settings file '{}' removed — dropping its mod", stem);
				_store.RemoveMod(stem);
			}
		}
		_schemaMtimes = std::move(seen);
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
		// Mirrors the runtime's _viewsSubscribers pruning: a view torn down by
		// crash-recovery must not keep receiving pushes for the process lifetime.
		_subscribers.erase(std::string(a_viewId));
	}

	void SettingsModule::PushHotkey(std::string_view a_modId, std::string_view a_key) const
	{
		PushToSubscribers("ui.hotkey", {
			{ "mod", std::string(a_modId) },
			{ "key", std::string(a_key) },
		});
	}

	void SettingsModule::PushToSubscribers(std::string_view a_type, const nlohmann::json& a_payload) const
	{
		if (!_bridge || _subscribers.empty()) {
			return;
		}
		_bridge->SendToWeb(_subscribers, a_type, a_payload);
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
			a_b.SendToWeb("settings.data", _store.DataView());
		});

		a_bridge.RegisterCommand("settings.set", [this](const nlohmann::json& a_payload, MessageBridge& a_b) {
			const auto mod = Json::GetString(a_payload, "mod", "");
			const auto key = Json::GetString(a_payload, "key", "");
			const auto valueIt = a_payload.find("value");
			// Ack shape (api-freeze-plan items 5 + 11): `value` is the
			// post-clamp committed value, so an unsubscribed caller learns what
			// was stored without a re-fetch and a subscribed one can tell
			// clamped from accepted. Failures carry a machine `code`.
			nlohmann::json ack = { { "mod", mod }, { "key", key } };
			if (valueIt == a_payload.end()) {
				ack["ok"] = false;
				ack["code"] = "invalid-value";
				ack["message"] = "missing value field";
			} else {
				const auto result = _store.SetValueWithResult(mod, key, *valueIt);
				ack["ok"] = result.ok;
				if (result.ok) {
					if (const auto* committed = _store.GetValue(mod, key)) {
						ack["value"] = *committed;
					}
				} else {
					ack["code"] = result.code;
				}
			}
			a_b.SendToWeb("settings.ack", ack);
		});

		a_bridge.RegisterCommand("settings.reset", [this](const nlohmann::json& a_payload, MessageBridge& a_b) {
			const auto mod = Json::GetString(a_payload, "mod", "");
			const auto key = Json::GetString(a_payload, "key", "");
			// Suppress the per-key settings.changed fan-out for the web: the
			// settings.data below syncs every subscriber, and a whole-mod reset
			// would otherwise send N redundant messages first. The core change
			// listener (native reactions) still fires per key.
			_suppressChangedPush = true;
			const bool ok = _store.Reset(mod, key);
			_suppressChangedPush = false;
			if (!ok) {
				// Item 5: a request-carrying caller gets ui.result;
				// fire-and-forget stays silent.
				a_b.SendResult(false, "unknown-setting", "unknown mod or setting (or a requires-gated stub)");
				return;
			}
			const auto& data = _store.DataView();
			// The caller's copy goes through the reply path, which echoes the
			// requestId so osfui.request("settings.reset") resolves with the
			// document; other subscribers get the plain push.
			const std::string caller(a_b.CurrentSource());
			for (const auto& id : _subscribers) {
				if (id != caller) {
					_bridge->SendToWeb(id, "settings.data", data);
				}
			}
			a_b.SendToWeb("settings.data", data);
		});
	}
}
