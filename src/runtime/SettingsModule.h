#pragma once

#include <unordered_set>  // not in pch.h

#include "runtime/SettingsStore.h"
#include "runtime/UiModule.h"

namespace OSFUI
{
	// The schema-driven settings feature as a self-contained module
	// (renderer-plan.md Phase 5). Owns the SettingsStore, registers the
	// settings.* bridge commands, and applies persisted values at startup.
	// Core knows nothing about it beyond the IUiModule contract — it could be
	// lifted into a separate plugin once a public registration API exists.
	//
	// Native reactions (a setting changing core behaviour) are NOT the module's
	// job: it just stores/validates/persists/notifies. Consumers inject a
	// ChangeListener and react to the keys they own (e.g. the runtime reacting
	// to its own cursor-speed knob).
	//
	// Web change delivery (mcm-design.md §8.5): `settings.get` SUBSCRIBES the
	// calling view — every later committed value is pushed to it as
	// `settings.changed { mod, key, value }`, a registry shape change
	// (runtime schema registration/removal) re-sends the full `settings.data`,
	// and a landed write-behind disk write pushes `settings.persisted { mod }`
	// (save feedback). Subscribe-on-read, the `views.get` pattern; a mod's own
	// HUD reacts live to its settings with zero polling and zero native code.
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

	private:
		// Sends one { type, payload } to every subscribed view (no-op with no
		// bridge or no subscribers — e.g. during the OnStart NotifyAll replay).
		void PushToSubscribers(std::string_view a_type, const nlohmann::json& a_payload) const;

		SettingsStore                   _store;
		std::filesystem::path           _schemaDir;
		std::filesystem::path           _valuesDir;
		MessageBridge*                  _bridge{ nullptr };  // set by RegisterCommands, cleared by OnBridgeDown
		std::unordered_set<std::string> _subscribers;        // view ids that sent settings.get
		bool                            _suppressChangedPush{ false };  // reset in flight: settings.data supersedes per-key pushes
	};
}
