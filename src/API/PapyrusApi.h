#pragma once

#include <cstdint>
#include <string>
#include <string_view>
#include <variant>
#include <vector>

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
	// The Papyrus script OSF UI binds its own natives on. Those natives take
	// the target mod id as a plain ARGUMENT and are trusted by construction —
	// Papyrus is a mod's own code — so they carry no caller check. Anything
	// that lets untrusted view content name a script has to refuse this one, or
	// it hands a page a trusted alias for the endpoints it is refused.
	inline constexpr std::string_view kPlatformScriptName = "OSFUI";

	// Main thread, once GameVM exists: bind the OSFUI script natives and install
	// the TESLoadGameEvent sink that re-binds them and clears session
	// registrations after a load. Runtime hands kPostDataLoad off to its tick;
	// the disabled-runtime path queues this operation directly. Idempotent.
	void Install();

	// Main thread (Runtime's store change listener): fan a committed settings
	// value out to matching registered script callbacks. Called for every
	// commit; a no-op while nothing is registered.
	void OnSettingChanged(std::string_view a_modId, std::string_view a_key);

	// Main thread (Runtime::DrainHotkeys): fan a dispatched hotkey out to
	// matching registered script callbacks.
	void OnHotkey(std::string_view a_modId, std::string_view a_key);

	enum class StaticDispatchResult
	{
		kQueued,
		kVmUnavailable,
		kTargetRejected,
		kCapacityReached,
	};
	// Queue one schema-owned GLOBAL callback without adding it to the
	// session-scoped registration table. The caller owns diagnostics/lifecycle.
	StaticDispatchResult DispatchStaticHotkey(std::string_view a_script,
		std::string_view a_function, std::string_view a_modId, std::string_view a_key);

	// Runtime's `papyrus.call` endpoint: queue an arbitrary GLOBAL function on
	// an arbitrary loose PEX. Values retain their JavaScript scalar types.
	using StaticCallArg = std::variant<std::string, std::int32_t, float, bool>;
	StaticDispatchResult DispatchStaticFunction(std::string_view a_script,
		std::string_view a_function, const std::vector<StaticCallArg>& a_args);

	// Main thread (Runtime's `papyrus.send` endpoint): fan a view-fired message
	// out to the mod's ListenForViewActions callbacks as
	// OnOSFUIViewAction(string asName, string[] asArgs). a_modId is derived
	// from the source view id by the caller, never the payload, and matched
	// case-insensitively. Fire-and-forget: no return value, no callback
	// functor, no RPC into the VM — a script that owes the view an answer
	// registers through ListenForViewRequests instead.
	void OnViewAction(std::string_view a_modId, std::string_view a_action, const std::vector<std::string>& a_args);

	// Main thread: dispatch one correlated request from an owning view to its
	// registered Papyrus listener. The callback receives (request, string[] args,
	// replyToken). False means no listener/capacity and nothing was dispatched.
	bool OnViewRequest(std::string_view a_modId, std::string_view a_request,
		const std::vector<std::string>& a_args, std::string_view a_viewId, std::string_view a_deferToken);

	struct ViewReply
	{
		std::string    view;
		std::string    deferToken;
		bool           rejected{ false };
		std::string    code;
		std::string    message;
		nlohmann::json value;
	};

	// Main thread: emit answered requests and expire unanswered ones. Successful
	// replies carry value; failures carry a stable code/message. Tokens are
	// OSF UI runtime-owned, one-shot, capped, and session-scoped.
	void DrainViewReplies(const std::function<void(const ViewReply&)>& a_deliver,
		std::chrono::steady_clock::time_point a_now = std::chrono::steady_clock::now());

	// One drained SetView* value. mod is ASCII-lowercased because BSFixedString
	// interning does not preserve caller casing; delivery matches opaque mod ids
	// case-insensitively. `value` is
	// the COMPLETE current value for the key, never a delta — a forms value is
	// serialized into it at drain time (identity objects with null slots
	// preserved), because form field reads are
	// main-thread-only while the queue holds FormIDs.
	struct ViewState
	{
		std::string    mod;
		std::string    key;
		nlohmann::json value;
	};

	// One drained SendViewEvent: a one-shot happening for the mod's instantiated views.
	// Never retained and never replayed — that is the whole distinction from
	// ViewState, and encoding a happening as state is the bug it prevents.
	struct ViewEvent
	{
		std::string              mod;
		std::string              name;
		std::vector<std::string> args;
	};

	// Main thread (Runtime::Tick, next to DrainSettingsOps): hand each queued
	// SetView* value to a_deliver, which retains it in the runtime's shared
	// RetainedStateStore and publishes it to the mod's instantiated views.
	void DrainViewState(const std::function<void(const ViewState&)>& a_deliver);

	// Main thread: hand each queued SendViewEvent to a_deliver for delivery to
	// the mod's instantiated views. Nothing is cached.
	void DrainViewEvents(const std::function<void(const ViewEvent&)>& a_deliver);

	// True once after a game load, so the caller can drop retained Papyrus
	// state: it holds session-scoped form identities that do not survive a
	// load. The retained cache lives in the runtime, not here.
	[[nodiscard]] bool TakeSessionReset();

	// Main thread (Runtime::Tick): apply queued Papyrus Set*/Reset ops through
	// the store's validated/clamped path. Refusals are logged, never thrown —
	// the setters are documented fire-and-forget.
	void DrainSettingsOps(SettingsStore& a_store);
}
