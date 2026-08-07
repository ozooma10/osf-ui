#pragma once

#include <unordered_set>  // not in pch.h

#include "runtime/SettingsStore.h"
#include "runtime/UiModule.h"

namespace OSFUI
{
	// The schema-driven settings feature as a self-contained module: owns the
	// SettingsStore, registers the settings.* bridge endpoints, and applies
	// persisted values at startup. The runtime holds it by `unique_ptr<IUiModule>`
	// in `_modules` and keeps a non-owning `SettingsModule*` beside it, so it is
	// both driven through the shared IUiModule lifecycle and reached through
	// concretely for schema facts.
	//
	// Native reactions are the consumer's job; the module only stores/
	// validates/persists/notifies. Inject a ChangeListener and react to the
	// keys you own (e.g. the runtime's cursor-speed knob).
	//
	// Web change delivery is explicit 2.0 state plus events: every greeted view
	// receives the current `osfui/settings` document during state replay; later
	// commits emit `settings.changed { mod, key, value }`, registry-shape changes
	// republish the document, and a landed write-behind disk write emits
	// `settings.persisted { mod }`.
	class SettingsModule final : public IUiModule
	{
	public:
		// a_legacyKeyMigrator (optional) must be handed in here rather than set
		// on the store afterwards: LoadAll runs inside this constructor and the
		// v1 -> v2 key-value migration happens while values files load.
		SettingsModule(std::filesystem::path a_schemaDir,
			std::filesystem::path a_valuesDir,
			SettingsStore::ChangeListener a_onChange,
			SettingsStore::LegacyKeyMigrator a_legacyKeyMigrator = {});

		void OnStart() override;  // apply persisted values (fires reactions)
		void RegisterEndpoints(MessageBridge& a_bridge) override;
		void OnBridgeDown() override;
		void OnViewDestroyed(std::string_view a_viewId) override;
		[[nodiscard]] std::string_view Name() const override { return "settings"; }

		// The store is the single source of truth every settings consumer projects
		// over — the native plugin API (runtime schema registration, typed
		// getters) reaches it through here.
		[[nodiscard]] SettingsStore& Store() { return _store; }

		// Web hotkey delivery: emits `ui.hotkey {mod, key}` to every greeted view;
		// a receiving view filters on payload.mod. Called by
		// Runtime::DrainHotkeys, main thread.
		void PushHotkey(std::string_view a_modId, std::string_view a_key) const;

		// Schema hot-reload (developer mode): mtime-polls
		// settings/*.json on a ~1 s cadence — a changed or new file reloads/
		// registers through the store (values preserved, §11 aliases honored,
		// registry re-broadcast pushes fresh `osfui/settings` state to greeted views); a
		// deleted file removes its mod, but only a drop-in one (a runtime
		// registration tracks no files). The caller gates on effective developer mode and passes
		// its monotonic clock (Runtime::Tick uptime, like PumpPersistence). The
		// mtime snapshot is seeded at construction, so the first pump reloads
		// nothing.
		static constexpr double kHotReloadScanSeconds = 1.0;
		void PumpSchemaHotReload(double a_nowSeconds);

		// Republish the whole settings document — for changes the store's own
		// listeners can't see (for example, a live ControlMap projection changing;
		// conflict annotations live in Data() but do not alter registry shape).
		// No-op with no bridge. Main thread.
		void BroadcastData();

	private:

		// stem -> last seen write time, recorded per attempt whether or not it
		// parsed: a half-written editor save fails to parse but its final write
		// bumps the mtime again, so it retries; a broken file logs once per save
		// instead of once per scan.
		using SchemaMtimes = std::unordered_map<std::string, std::filesystem::file_time_type>;
		[[nodiscard]] SchemaMtimes ScanSchemaDir() const;

		SettingsStore                   _store;
		std::filesystem::path           _schemaDir;
		std::filesystem::path           _valuesDir;
		MessageBridge*                  _bridge{ nullptr };  // set by RegisterEndpoints, cleared by OnBridgeDown
		bool                            _suppressChangedPush{ false };  // reset in flight: `osfui/settings` state supersedes per-key pushes
		SchemaMtimes                    _schemaMtimes;       // hot-reload snapshot (seeded in the ctor)
		double                          _nextSchemaScan{ 0.0 };
	};
}
