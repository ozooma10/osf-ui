#pragma once

#include <cstdint>
#include <string>
#include <string_view>
#include <variant>
#include <vector>

#include <nlohmann/json.hpp>

namespace RE::BSScript
{
	class IVirtualMachine;
}

namespace OSFUI::API::Papyrus
{
	// Block page requests from calling this platform script directly.
	inline constexpr std::string_view kPlatformScriptName = "OSFUI";

	// Once per process. Normally called from the GameVM ctor hook; Install() is the fallback.
	void BindNatives(RE::BSScript::IVirtualMachine& a_vm);

	// Runtime update; idempotent. Binds natives if unbound and installs the session sinks.
	void Install();

	// Any thread. Ends the script session exactly as a game load does.
	void OnMainMenuOpened();

	enum class StaticDispatchResult
	{
		kQueued,
		kVmUnavailable,
		kTargetRejected,
	};
	// Loose-PEX Global call; preserves JavaScript scalar types.
	using StaticCallArg = std::variant<std::string, std::int32_t, float, bool>;
	StaticDispatchResult DispatchStaticFunction(std::string_view a_script, std::string_view a_function, const std::vector<StaticCallArg>& a_args);

	// Form IDs resolve only when building a VM callback and serialize only on main.
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

	// Exact "modId.name" match; never split at a dot.
	[[nodiscard]] ViewEndpoint ResolveViewEndpoint(std::string_view a_name);

	bool OnViewSend(std::string_view a_modId, std::string_view a_name, const std::vector<Value>& a_args, std::string_view a_sourceViewId);
	// a_deferToken is MessageBridge::Defer()'s token; the script receives it as its reply token.
	StaticDispatchResult OnViewRequest(std::string_view a_modId, std::string_view a_name, const std::vector<Value>& a_args, std::string_view a_sourceViewId, std::uint64_t a_deferToken);

	struct ViewReply
	{
		std::uint64_t  token{ 0 };  // MessageBridge::Defer()'s token
		bool           rejected{ false };
		std::string    code;
		std::string    message;
		nlohmann::json value;
	};

	// Lower-cased mod id; FormIDs serialized in the runtime update.
	struct ViewState
	{
		std::string    mod;
		std::string    key;
		nlohmann::json value;
	};

	// Never retained or replayed.
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

	[[nodiscard]] PendingBatch TakePendingBatch();

}
