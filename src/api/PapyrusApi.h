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

	// MAIN thread (Runtime's `ui.action` bridge command): fan a view-fired
	// action out to the mod's RegisterForViewActions callbacks as
	// asFn(asAction, asArg). a_modId is derived from the SOURCE view id by the
	// caller (never the payload) and matched case-insensitively. Fire-and-
	// forget: no return value, no callback functor — deliberately no RPC into
	// the VM (docs/authoring-dynamic-data.md).
	void OnViewAction(std::string_view a_modId, std::string_view a_action, std::string_view a_arg);

	// One queued PushToView payload. mod is canonical lowercase (folded from
	// the interned Papyrus string and validated against the id grammar), so
	// delivery can prefix-match it against lowercase-by-grammar view ids.
	struct ViewPush
	{
		std::string              mod;
		std::string              key;
		std::vector<std::string> values;
	};

	// MAIN thread (Runtime::Tick, next to DrainSettingsOps): hand each queued
	// Papyrus PushToView payload to a_deliver, which fans it out to the mod's
	// live views as `data.push`. Fire-and-forget end to end — nothing is
	// cached natively; a view that (re)opens fires a `ready` action and the
	// script re-pushes current state.
	void DrainViewPushes(const std::function<void(const ViewPush&)>& a_deliver);

	// MAIN thread (Runtime::Tick): apply queued Papyrus Set*/Reset ops through
	// the store's validated/clamped path. Refusals are logged, never thrown —
	// the setters are documented fire-and-forget.
	void DrainSettingsOps(SettingsStore& a_store);
}
