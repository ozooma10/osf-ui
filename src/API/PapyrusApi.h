#pragma once

#include <chrono>
#include <cstdint>
#include <string>
#include <string_view>
#include <variant>
#include <vector>

#include <nlohmann/json.hpp>

namespace OSFUI::API::Papyrus
{
	// Untrusted script dispatch must reject this trusted platform script.
	inline constexpr std::string_view kPlatformScriptName = "OSFUI";
	inline constexpr std::string_view kViewScriptName = "OSFUI_View";

	// Main thread and idempotent; binds natives and installs game-load cleanup.
	void Install();

	enum class StaticDispatchResult
	{
		kQueued,
		kVmUnavailable,
		kTargetRejected,
		kCapacityReached,
	};
	// Queue a loose-PEX GLOBAL call while preserving JavaScript scalar types.
	using StaticCallArg = std::variant<std::string, std::int32_t, float, bool>;
	StaticDispatchResult DispatchStaticFunction(std::string_view a_script, std::string_view a_function, const std::vector<StaticCallArg>& a_args);

	// Portable scalar carried by the JavaScript/Papyrus bridge. Form IDs are resolved only while building a VM callback and serialized only on main.
	struct FormValue
	{
		std::uint32_t id{ 0 };
	};
	using Value = std::variant<std::monostate, bool, std::int32_t, float, std::string, FormValue>;

	enum class ViewEndpointKind : std::uint8_t
	{
		kNone,
		kSend,
		kRequest,
	};

	struct ViewEndpoint
	{
		ViewEndpointKind kind{ ViewEndpointKind::kNone };
		std::string      modId;
		std::string      name;
	};

	// A local name is resolved against a_sourceModId first; otherwise the bounded registry is matched against each exact "modId.name" qualified endpoint.
	[[nodiscard]] ViewEndpoint ResolveViewEndpoint(std::string_view a_sourceModId, std::string_view a_name);

	bool OnViewSend(std::string_view a_modId, std::string_view a_name, const std::vector<Value>& a_args, std::string_view a_sourceViewId);
	bool OnViewRequest(std::string_view a_modId, std::string_view a_name, const std::vector<Value>& a_args, std::string_view a_sourceViewId, std::string_view a_deferToken);

	struct ViewReply
	{
		std::string    view;
		std::string    deferToken;
		bool           rejected{ false };
		std::string    code;
		std::string    message;
		nlohmann::json value;
	};

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
		std::string    mod;
		std::string    name;
		nlohmann::json args;
	};

	struct PendingBatch
	{
		std::vector<ViewState>         states;
		std::vector<ViewEvent>         events;
		std::vector<ViewReply>         replies;
		bool                           sessionReset{ false };
	};

	[[nodiscard]] PendingBatch TakePendingBatch(std::chrono::steady_clock::time_point a_now = std::chrono::steady_clock::now());

}
