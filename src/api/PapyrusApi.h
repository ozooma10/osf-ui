#pragma once

#include <nlohmann/json.hpp>

namespace OSFUI
{
	class SettingsStore;
}

// Natives behind the shipped `OSFUI.psc` (data/Scripts/Source). Getters read
// the any-thread SettingsMirror, like the C ABI typed getters — never
// SettingsStore, which is main-thread only. Setters/resets enqueue here and
// Runtime::Tick drains them through the validated SettingsStore::Set path, so
// Papyrus gets no bypass. Change/hotkey events dispatch to registered script
// callbacks over the VM's async call queue (DispatchMethodCall/
// DispatchStaticCall), so delivery never blocks the main thread.
//
// Registrations are session-scoped: a game load resets the VM, so the registry
// clears on TESLoadGameEvent (stored receiver pointers would dangle) and
// scripts re-register from their own load-game handling.
namespace OSFUI::API::Papyrus
{
	// Main thread, once GameVM exists (SFSE kPostDataLoad): bind the OSFUI
	// script natives and install the TESLoadGameEvent sink that re-binds them
	// and clears session registrations after a load. Idempotent.
	void Install();

	// Main thread (Runtime's store change listener): fan a committed settings
	// value out to matching registered script callbacks. Called for every
	// commit; a no-op while nothing is registered.
	void OnSettingChanged(std::string_view a_modId, std::string_view a_key);

	// Main thread (Runtime::DrainHotkeys): fan a dispatched hotkey out to
	// matching registered script callbacks.
	void OnHotkey(std::string_view a_modId, std::string_view a_key);

	// Main thread (Runtime's `ui.action` bridge command): fan a view-fired
	// action out to the mod's RegisterForViewActions callbacks. a_modId is
	// derived from the source view id by the caller, never the payload, and
	// matched case-insensitively. a_args is the action's argument list (the
	// legacy scalar `arg` arrives as a one-element vector): scalar-arg
	// registrants get asFn(asAction, a_args[0]-or-""), args-list registrants
	// (RegisterForViewActionsArgs) get asFn(asAction, string[]). Fire-and-
	// forget: no return value, no callback functor, no RPC into the VM.
	void OnViewAction(std::string_view a_modId, std::string_view a_action, const std::vector<std::string>& a_args);

	// One drained PushToView/PushFormsToView payload. mod is canonical
	// lowercase (folded from the interned Papyrus string and validated against
	// the id grammar), so delivery can prefix-match it against
	// lowercase-by-grammar view ids.
	struct ViewPush
	{
		std::string              mod;
		std::string              key;
		std::vector<std::string> values;
		// PushFormsToView only (protocol 1.3): the serialized `forms` array for
		// the data.push payload — identity objects with null slots preserved
		// (docs/form-references-design.md). Built at drain time on the main
		// thread (form field reads are main-thread-only; the queue holds
		// FormIDs). has_value() distinguishes an EMPTY forms push ("the list is
		// now empty") from a plain PushToView, which omits the field entirely.
		std::optional<nlohmann::json> forms;
	};

	// Main thread (Runtime::Tick, next to DrainSettingsOps): hand each queued
	// Papyrus PushToView payload to a_deliver, which fans it out to the mod's
	// live views as `data.push`. Fire-and-forget end to end — nothing is cached
	// natively; a view that (re)opens fires a `ready` action and the script
	// re-pushes current state.
	void DrainViewPushes(const std::function<void(const ViewPush&)>& a_deliver);

	// Main thread (Runtime::Tick): apply queued Papyrus Set*/Reset ops through
	// the store's validated/clamped path. Refusals are logged, never thrown —
	// the setters are documented fire-and-forget.
	void DrainSettingsOps(SettingsStore& a_store);
}
