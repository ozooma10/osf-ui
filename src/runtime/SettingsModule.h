#pragma once

#include "runtime/SettingsStore.h"
#include "runtime/UiModule.h"

namespace SWUI
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
	class SettingsModule final : public IUiModule
	{
	public:
		SettingsModule(std::filesystem::path a_schemaDir,
			std::filesystem::path a_valuesDir,
			SettingsStore::ChangeListener a_onChange);

		void OnStart() override;  // apply persisted values (fires reactions)
		void RegisterCommands(MessageBridge& a_bridge) override;
		[[nodiscard]] std::string_view Name() const override { return "settings"; }

	private:
		SettingsStore         _store;
		std::filesystem::path _schemaDir;
		std::filesystem::path _valuesDir;
	};
}
