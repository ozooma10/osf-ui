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

// Getters use the any-thread mirror; mutations drain on main, and registrations reset on game load.
namespace OSFUI::API::Papyrus
{
	// Untrusted script dispatch must reject this trusted platform script.
	inline constexpr std::string_view kPlatformScriptName = "OSFUI";

	// Main thread and idempotent; binds natives and installs game-load cleanup.
	void Install();

	// Main thread; dispatches a committed setting to matching script callbacks.
	void OnSettingChanged(std::string_view a_modId, std::string_view a_key);

	// Main thread; dispatches a hotkey to matching script callbacks.
	void OnHotkey(std::string_view a_modId, std::string_view a_key);

	enum class StaticDispatchResult
	{
		kQueued,
		kVmUnavailable,
		kTargetRejected,
		kCapacityReached,
	};
	// Queue one schema-owned GLOBAL callback; the caller owns diagnostics and lifecycle.
	StaticDispatchResult DispatchStaticHotkey(std::string_view a_script,
		std::string_view a_function, std::string_view a_modId, std::string_view a_key);

	// Queue a loose-PEX GLOBAL call while preserving JavaScript scalar types.
	using StaticCallArg = std::variant<std::string, std::int32_t, float, bool>;
	StaticDispatchResult DispatchStaticFunction(std::string_view a_script,
		std::string_view a_function, const std::vector<StaticCallArg>& a_args);

	// Main thread; fire-and-forget to source-derived mod listeners.
	void OnViewAction(std::string_view a_modId, std::string_view a_action, const std::vector<std::string>& a_args);

	// Main thread; returns false when no listener or request capacity is available.
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

	// Main thread; drains one-shot session replies and expires unanswered requests.
	void DrainViewReplies(const std::function<void(const ViewReply&)>& a_deliver,
		std::chrono::steady_clock::time_point a_now = std::chrono::steady_clock::now());

	// Drained retained state with lowercase mod id; FormIDs are serialized on the main thread.
	struct ViewState
	{
		std::string    mod;
		std::string    key;
		nlohmann::json value;
	};

	// A one-shot event for instantiated views; never retained or replayed.
	struct ViewEvent
	{
		std::string              mod;
		std::string              name;
		std::vector<std::string> args;
	};

	// Main thread; hands queued retained state to the runtime.
	void DrainViewState(const std::function<void(const ViewState&)>& a_deliver);

	// Main thread; hands queued events to the runtime without caching them.
	void DrainViewEvents(const std::function<void(const ViewEvent&)>& a_deliver);

	// True once after game load so the runtime drops session-scoped retained state.
	[[nodiscard]] bool TakeSessionReset();

	// Main thread; applies queued mutations through the validated store path.
	void DrainSettingsOps(SettingsStore& a_store);
}
