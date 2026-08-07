#include "runtime/SettingsModule.h"

#include "runtime/Ids.h"
#include "runtime/Json.h"
#include "runtime/MessageBridge.h"

namespace OSFUI
{
	SettingsModule::SettingsModule(std::filesystem::path a_schemaDir,
		std::filesystem::path a_valuesDir,
		SettingsStore::ChangeListener a_onChange,
		SettingsStore::LegacyKeyMigrator a_legacyKeyMigrator) :
		_schemaDir(std::move(a_schemaDir)),
		_valuesDir(std::move(a_valuesDir))
	{
		// Before LoadAll below — the v1 -> v2 key-value migration runs while
		// values files load.
		_store.SetLegacyKeyMigrator(std::move(a_legacyKeyMigrator));
		// Subscriber #0: the runtime's core reaction (framework knobs). Later
		// listeners multicast behind it.
		_store.AddChangeListener(std::move(a_onChange));
		// Subscriber #1: web event — every committed value goes to every greeted
		// view. The no-bridge guard
		// runs before the payload is built; startup NotifyAll and every set with
		// no view open would otherwise allocate json for nobody.
		_store.AddChangeListener([this](std::string_view a_mod, std::string_view a_key, const nlohmann::json& a_value) {
			if (_suppressChangedPush || !_bridge) {
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
			_bridge->EmitAll("settings.changed", payload);
		});
		// Registry SHAPE changed (native registration/removal while views are
		// live): republish the whole document. Individual value commits stay
		// `settings.changed` events, so this large payload only moves when the
		// set of mods or settings actually changes — which is rare.
		_store.AddRegistryListener([this] {
			if (_bridge) {
				_bridge->PublishStateAll("osfui", "settings", _store.DataView());
			}
		});
		// A mod's values-file write landed (the write-behind flush, distinct
		// from the immediate settings.changed commit): lets the settings UI
		// show "Saved" feedback.
		_store.AddPersistListener([this](std::string_view a_mod) {
			if (_bridge) {
				_bridge->EmitAll("settings.persisted", { { "mod", std::string(a_mod) } });
			}
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
		std::filesystem::directory_iterator it(
			_schemaDir, std::filesystem::directory_options::skip_permission_denied, ec);
		const std::filesystem::directory_iterator end;
		for (; it != end; it.increment(ec)) {
			const auto entry = *it;
			std::error_code entryEc;
			if (entry.is_regular_file(entryEc) && entry.path().extension() == ".json") {
				if (const auto t = entry.last_write_time(entryEc); !entryEc) {
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
		// `osfui/settings` state re-broadcast, HotkeyService rebuild via the registry
		// listener) rides the same wiring as a native registration. The mtime
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

	void SettingsModule::BroadcastData()
	{
		if (_bridge) {
			_bridge->PublishStateAll("osfui", "settings", _store.DataView());
		}
	}

	void SettingsModule::OnBridgeDown()
	{
		_bridge = nullptr;
	}

	void SettingsModule::OnViewDestroyed(std::string_view)
	{
		// Nothing to prune. Delivery is scoped by the bridge's own greeted-view
		// set, which a destroyed view leaves automatically — the 1.x subscriber
		// set had to be swept here or a crash-recovered view kept receiving
		// pushes for the process lifetime.
	}

	void SettingsModule::PushHotkey(std::string_view a_modId, std::string_view a_key) const
	{
		if (_bridge) {
			_bridge->EmitAll("ui.hotkey", {
				{ "mod", std::string(a_modId) },
				{ "key", std::string(a_key) },
			});
		}
	}

	void SettingsModule::RegisterEndpoints(MessageBridge& a_bridge)
	{
		_bridge = &a_bridge;

		// `settings.get` is gone. It was a request whose real job was to
		// SUBSCRIBE the caller, which is what state is for: the registry is
		// published as the `osfui/settings` state key and replayed to every
		// fresh document, so a view renders from it with no read roundtrip and
		// nothing to re-request after F5.

		a_bridge.RegisterRequest("settings.set", [this](const nlohmann::json& a_payload, MessageBridge& a_b) {
			const auto requested = Json::GetString(a_payload, "mod", "");
			// Authority check before anything else: only the built-in Mod Settings view
			// and Keybindings view may write a mod other than their own
			// (Ids::ResolveWritableMod). Without this, any bridged view could
			// rewrite a neighbour's settings — or OSF UI's own toggleKey, which is
			// the input layer's guaranteed way out of the overlay.
			const auto allowed = Ids::ResolveWritableMod(a_b.CurrentSource(), requested);
			if (!allowed) {
				REX::WARN("SettingsModule: [content] view '{}' refused settings.set for '{}' (not its own mod)",
					a_b.CurrentSource(), requested);
				a_b.Reject("forbidden", "a view may only write its own mod's settings");
				return;
			}
			const std::string mod(*allowed);
			const auto key = Json::GetString(a_payload, "key", "");
			const auto valueIt = a_payload.find("value");
			if (valueIt == a_payload.end()) {
				a_b.Reject("invalid-value", "missing value field");
				return;
			}
			// A failed set REJECTS with its code. 1.x resolved a
			// `settings.ack { ok:false }` the caller had to remember to inspect,
			// so forgetting read as success.
			const auto result = _store.SetValueWithResult(mod, key, *valueIt);
			if (!result.ok) {
				a_b.Reject(result.code, "the value was refused");
				return;
			}
			// `value` is the post-clamp COMMITTED value, so the caller can tell
			// clamped from accepted without a re-fetch.
			nlohmann::json reply = { { "mod", mod }, { "key", key } };
			if (const auto* committed = _store.GetValue(mod, key)) {
				reply["value"] = *committed;
			}
			a_b.Respond(reply);
		});

		a_bridge.RegisterRequest("settings.reset", [this](const nlohmann::json& a_payload, MessageBridge& a_b) {
			const auto requested = Json::GetString(a_payload, "mod", "");
			// Same authority check as settings.set — a reset is a write.
			const auto allowed = Ids::ResolveWritableMod(a_b.CurrentSource(), requested);
			if (!allowed) {
				REX::WARN("SettingsModule: [content] view '{}' refused settings.reset for '{}' (not its own mod)",
					a_b.CurrentSource(), requested);
				a_b.Reject("forbidden", "a view may only reset its own mod's settings");
				return;
			}
			const std::string mod(*allowed);
			const auto key = Json::GetString(a_payload, "key", "");
			// Suppress the per-key settings.changed fan-out for the web: the
			// state republish below syncs every view, and a whole-mod reset would
			// otherwise send N redundant events first. The core change listener
			// (native reactions) still fires per key.
			_suppressChangedPush = true;
			const bool ok = _store.Reset(mod, key);
			_suppressChangedPush = false;
			if (!ok) {
				a_b.Reject("unknown-setting", "unknown mod or setting (or a requires-gated stub)");
				return;
			}
			// The refreshed registry reaches EVERY view — including the caller —
			// as the `osfui/settings` state key. The reply says only "the reset
			// happened"; carrying the document in it as well would make the
			// caller's copy arrive by a different route than everyone else's.
			a_b.Respond(nlohmann::json::object());
			a_b.PublishStateAll("osfui", "settings", _store.DataView());
		});
	}
}
