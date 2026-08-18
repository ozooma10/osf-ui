#pragma once

#include <unordered_set>  // not in pch.h

#include "Settings/SettingsStore.h"

namespace OSFUI
{
	class MessageBridge;

	class SettingsModule final
	{
	public:
		SettingsModule(std::filesystem::path a_schemaDir, std::filesystem::path a_valuesDir, SettingsStore::ChangeListener a_onChange, SettingsStore::LegacyKeyMigrator a_legacyKeyMigrator = {});

		void OnStart();  // apply persisted values (fires reactions)
		void RegisterEndpoints(MessageBridge& a_bridge);
		void OnBridgeDown();

		[[nodiscard]] SettingsStore& Store() { return _store; }

		void PushHotkey(std::string_view a_modId, std::string_view a_key) const;

		static constexpr double kHotReloadScanSeconds = 1.0;
		void PumpSchemaHotReload(double a_nowSeconds);

		void BroadcastData();

	private:

		using SchemaMtimes = std::unordered_map<std::string, std::filesystem::file_time_type>;
		[[nodiscard]] std::optional<SchemaMtimes> ScanSchemaDir() const;

		SettingsStore                   _store;
		std::filesystem::path           _schemaDir;
		std::filesystem::path           _valuesDir;
		MessageBridge*                  _bridge{ nullptr };  // set by RegisterEndpoints, cleared by OnBridgeDown
		bool                            _suppressChangedPush{ false };  // reset in flight: `osfui/settings` state supersedes per-key pushes
		SchemaMtimes                    _schemaMtimes;       // hot-reload snapshot (seeded in the ctor)
		double                          _nextSchemaScan{ 0.0 };
	};
}
