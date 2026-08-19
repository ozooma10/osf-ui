#include "Settings/SettingsModule.h"

#include "Core/Ids.h"
#include "Core/Json.h"
#include "Bridge/MessageBridge.h"

namespace OSFUI
{
	SettingsModule::SettingsModule(std::filesystem::path a_schemaDir, std::filesystem::path a_valuesDir, SettingsStore::ChangeListener a_onChange, SettingsStore::LegacyKeyMigrator a_legacyKeyMigrator) :
		_schemaDir(std::move(a_schemaDir)), _valuesDir(std::move(a_valuesDir))
	{
		_store.SetLegacyKeyMigrator(std::move(a_legacyKeyMigrator));
		_store.AddChangeListener(std::move(a_onChange));
		_store.AddChangeListener([this](std::string_view a_mod, std::string_view a_key, const nlohmann::json& a_value) {
			if (_suppressChangedPush || !_bridge) {
				return;
			}
			nlohmann::json payload = {
				{ "mod", std::string(a_mod) },
				{ "key", std::string(a_key) },
				{ "value", a_value },
			};
			if (_store.GetSettingType(a_mod, a_key) == "key") {
				payload["conflicts"] = _store.ConflictsForSetting(a_mod, a_key);
			}
			_bridge->EmitAll("settings.changed", payload);
		});
		_store.AddRegistryListener([this] {
			if (_bridge) {
				_bridge->PublishStateAll("osfui", "settings", _store.DataView());
			}
		});
		_store.AddPersistListener([this](std::string_view a_mod) {
			if (_bridge) {
				_bridge->EmitAll("settings.persisted", { { "mod", std::string(a_mod) } });
			}
		});
		_store.LoadAll(_schemaDir, _valuesDir);
		if (auto scan = ScanSchemaDir()) {
			_schemaMtimes = std::move(*scan);
		}
	}

	std::optional<SettingsModule::SchemaMtimes> SettingsModule::ScanSchemaDir() const
	{
		SchemaMtimes seen;
		std::error_code ec;
		std::filesystem::directory_iterator it(_schemaDir, std::filesystem::directory_options::none, ec);
		const std::filesystem::directory_iterator end;
		if (ec) {
			REX::WARN("SettingsModule: cannot scan '{}': {}", _schemaDir.string(), ec.message());
			return std::nullopt;
		}

		while (it != end) {
			const auto entry = *it;
			const auto path = entry.path();
			std::error_code entryEc;
			const auto regular = entry.is_regular_file(entryEc);
			if (entryEc) {
				REX::WARN("SettingsModule: cannot inspect '{}': {}", path.string(), entryEc.message());
				return std::nullopt;
			}
			if (regular && path.extension() == ".json") {
				const auto mtime = entry.last_write_time(entryEc);
				if (entryEc) {
					REX::WARN("SettingsModule: cannot read timestamp for '{}': {}", path.string(), entryEc.message());
					return std::nullopt;
				}
				seen.emplace(path.stem().string(), mtime);
			}

			it.increment(ec);
			if (ec) {
				REX::WARN("SettingsModule: scan of '{}' failed: {}", _schemaDir.string(), ec.message());
				return std::nullopt;
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

		auto scan = ScanSchemaDir();
		if (!scan) {
			return;  // preserve the registry and last complete snapshot
		}
		auto seen = std::move(*scan);
		for (const auto& [stem, mtime] : seen) {
			const auto it = _schemaMtimes.find(stem);
			if (it == _schemaMtimes.end() || it->second != mtime) {
				_store.ReloadDropInFile(_schemaDir / (stem + ".json"));
			}
		}
		// A deleted file removes its mod. Values files are kept (§10).
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

		a_bridge.RegisterRequest("settings.set", [this](const nlohmann::json& a_payload, MessageBridge& a_b) {
			const auto requested = Json::Get(a_payload, "mod", "");
			const auto allowed = Ids::ResolveWritableMod(a_b.CurrentSource(), requested);
			if (!allowed) {
				REX::WARN("SettingsModule: [content] view '{}' refused settings.set for '{}' (not its own mod)", a_b.CurrentSource(), requested);
				a_b.Reject("forbidden", "a view may only write its own mod's settings");
				return;
			}
			const std::string mod(*allowed);
			const auto key = Json::Get(a_payload, "key", "");
			const auto valueIt = a_payload.find("value");
			if (valueIt == a_payload.end()) {
				a_b.Reject("invalid-value", "missing value field");
				return;
			}
			const auto result = _store.SetValueWithResult(mod, key, *valueIt);
			if (!result.ok) {
				a_b.Reject(result.code, "the value was refused");
				return;
			}
			nlohmann::json reply = { { "mod", mod }, { "key", key } };
			if (const auto* committed = _store.GetValue(mod, key)) {
				reply["value"] = *committed;
			}
			a_b.Respond(reply);
		});

		a_bridge.RegisterRequest("settings.reset", [this](const nlohmann::json& a_payload, MessageBridge& a_b) {
			const auto requested = Json::Get(a_payload, "mod", "");
			// Same authority check as settings.set — a reset is a write.
			const auto allowed = Ids::ResolveWritableMod(a_b.CurrentSource(), requested);
			if (!allowed) {
				REX::WARN("SettingsModule: [content] view '{}' refused settings.reset for '{}' (not its own mod)", a_b.CurrentSource(), requested);
				a_b.Reject("forbidden", "a view may only reset its own mod's settings");
				return;
			}
			const std::string mod(*allowed);
			const auto key = Json::Get(a_payload, "key", "");

			_suppressChangedPush = true;
			const bool ok = _store.Reset(mod, key);
			_suppressChangedPush = false;
			if (!ok) {
				a_b.Reject("unknown-setting", "unknown mod or setting (or a requires-gated stub)");
				return;
			}

			a_b.Respond(nlohmann::json::object());
			a_b.PublishStateAll("osfui", "settings", _store.DataView());
		});
	}
}
