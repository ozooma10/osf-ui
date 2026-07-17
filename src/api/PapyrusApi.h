#pragma once

namespace OSFUI
{
	class SettingsStore;
}

// Papyrus consumption surface (mcm-design.md §8.4): the natives behind the
// shipped `OSFUI.psc` (data/Scripts/Source). Getters read the same any-thread
// SettingsMirror as the C ABI typed getters — never SettingsStore, which is
// main-thread-only. Setters/resets enqueue here and Runtime::Tick drains them
// through the normal validated SettingsStore::Set path — Papyrus gets no
// bypass. Change/hotkey events dispatch to registered script callbacks over
// the VM's async call queue (DispatchMethodCall/DispatchStaticCall — the
// proven OSF Animation SceneEventRelay transport), so delivery never blocks
// the main thread.
//
// Registrations are SESSION-scoped: a game load resets the VM, so the
// registry clears on TESLoadGameEvent (stored receiver pointers would dangle)
// and scripts re-register from their own load-game handling. Author docs:
// docs/authoring-settings.md "From Papyrus" + the psc's own comments.
namespace OSFUI::API::Papyrus
{
	// MAIN thread, once GameVM exists (Plugin, SFSE kPostDataLoad): bind the
	// OSFUI script natives and install the TESLoadGameEvent sink that re-binds
	// them + clears session registrations after a load. Idempotent.
	void Install();

	// MAIN thread (Runtime's store change listener): fan a committed settings
	// value out to matching registered script callbacks. Called for every
	// commit; a no-op while nothing is registered.
	void OnSettingChanged(std::string_view a_modId, std::string_view a_key);

	// MAIN thread (Runtime::DrainHotkeys): fan a dispatched hotkey out to
	// matching registered script callbacks.
	void OnHotkey(std::string_view a_modId, std::string_view a_key);

	// MAIN thread (Runtime::Tick): apply queued Papyrus Set*/Reset ops through
	// the store's validated/clamped path. Refusals are logged, never thrown —
	// the setters are documented fire-and-forget.
	void DrainSettingsOps(SettingsStore& a_store);
}
