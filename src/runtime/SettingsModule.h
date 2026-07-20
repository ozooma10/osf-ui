#pragma once

#include <unordered_set>  // not in pch.h

#include "runtime/SettingsStore.h"
#include "runtime/UiModule.h"

namespace OSFUI
{
	// The schema-driven settings feature as a self-contained module: owns the
	// SettingsStore, registers the settings.* bridge commands, applies
	// persisted values at startup. Core knows nothing about it beyond the
	// IUiModule contract.
	//
	// Native reactions are the consumer's job; the module only stores/
	// validates/persists/notifies. Inject a ChangeListener and react to the
	// keys you own (e.g. the runtime's cursor-speed knob).
	//
	// Web change delivery (mcm-design.md §8.5) is subscribe-on-read, the
	// `views.get` pattern: `settings.get` subscribes the calling view — every
	// later committed value is pushed to it as `settings.changed { mod, key,
	// value }`, a registry shape change (runtime schema registration/removal)
	// re-sends the full `settings.data`, and a landed write-behind disk write
	// pushes `settings.persisted { mod }`.
	class SettingsModule final : public IUiModule
	{
	public:
		SettingsModule(std::filesystem::path a_schemaDir,
			std::filesystem::path a_valuesDir,
			SettingsStore::ChangeListener a_onChange);

		void OnStart() override;  // apply persisted values (fires reactions)
		void RegisterCommands(MessageBridge& a_bridge) override;
		void OnBridgeDown() override;
		void OnViewDestroyed(std::string_view a_viewId) override;  // drop it from _subscribers
		[[nodiscard]] std::string_view Name() const override { return "settings"; }

		// The store is the single source of truth every other surface projects
		// over — the native plugin API (runtime schema registration, typed
		// getters) reaches it through here.
		[[nodiscard]] SettingsStore& Store() { return _store; }

		// Web hotkey delivery (mcm-design.md §9): pushes `ui.hotkey {mod, key}`
		// to every settings.get subscriber — the same set that gets
		// settings.changed; a receiving view filters on payload.mod. Called by
		// Runtime::DrainHotkeys, main thread.
		void PushHotkey(std::string_view a_modId, std::string_view a_key) const;

		// Schema hot-reload (mcm-design.md §12.1, dev mode): mtime-polls
		// settings/*.json on a ~1 s cadence — a changed or new file reloads/
		// registers through the store (values preserved, §11 aliases honored,
		// registry re-broadcast pushes fresh settings.data to subscribers); a
		// deleted file removes its mod, but only a drop-in one (a runtime
		// registration tracks no files). The caller gates on devMode and passes
		// its monotonic clock (Runtime::Tick uptime, like PumpPersistence). The
		// mtime snapshot is seeded at construction, so the first pump reloads
		// nothing.
		static constexpr double kHotReloadScanSeconds = 1.0;
		void PumpSchemaHotReload(double a_nowSeconds);

		// Re-send the full settings document to every subscriber — for changes
		// the store's own listeners can't see (e.g. the vanilla-keys table
		// flipping, api-freeze-plan item 7: the conflict annotations live in
		// Data() but SetVanillaKeys bumps no generation). No-op with no bridge
		// or no subscribers. Main thread.
		void BroadcastData() { PushToSubscribers("settings.data", _store.DataView()); }

	private:
		// Sends one { type, payload } to every subscribed view (no-op with no
		// bridge or no subscribers — e.g. during the OnStart NotifyAll replay).
		void PushToSubscribers(std::string_view a_type, const nlohmann::json& a_payload) const;

		// stem -> last seen write time, recorded per attempt whether or not it
		// parsed: a half-written editor save fails to parse but its final write
		// bumps the mtime again, so it retries; a broken file logs once per save
		// instead of once per scan.
		using SchemaMtimes = std::unordered_map<std::string, std::filesystem::file_time_type>;
		[[nodiscard]] SchemaMtimes ScanSchemaDir() const;

		SettingsStore                   _store;
		std::filesystem::path           _schemaDir;
		std::filesystem::path           _valuesDir;
		MessageBridge*                  _bridge{ nullptr };  // set by RegisterCommands, cleared by OnBridgeDown
		std::unordered_set<std::string> _subscribers;        // view ids that sent settings.get
		bool                            _suppressChangedPush{ false };  // reset in flight: settings.data supersedes per-key pushes
		SchemaMtimes                    _schemaMtimes;       // hot-reload snapshot (seeded in the ctor)
		double                          _nextSchemaScan{ 0.0 };
	};
}
