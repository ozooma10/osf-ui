#pragma once

#include <nlohmann/json.hpp>

// Narrow native <-> web bridge. All traffic is JSON text messages with the
// shape { "type": string, "payload": object }. Web -> native traffic is a
// single type, `ui.command`, dispatched by its `command` field against a
// registry of handlers. The bridge itself is FEATURE-AGNOSTIC: core registers
// platform commands (close/setVisible/...), and each IUiModule registers its
// own (e.g. settings.*). There is intentionally NO mechanism to call arbitrary
// native functions from JS. See docs/security-model.md.

namespace PrismaSF
{
	class MessageBridge
	{
	public:
		// Transport for native -> web text: deliver `a_json` to view `a_viewId`.
		using SendFn = std::function<void(std::string_view a_viewId, std::string_view a_json)>;

		// Handler for one `ui.command` command. Receives the command payload
		// and the bridge (to send responses). Registered by core/modules.
		using CommandHandler = std::function<void(const nlohmann::json& a_payload, MessageBridge& a_bridge)>;

		explicit MessageBridge(SendFn a_send);

		// Register (or replace) the handler for an exact command string, e.g.
		// "settings.get". Unknown commands are rejected and logged.
		void RegisterCommand(std::string a_command, CommandHandler a_handler);

		// Entry point for web -> native messages (raw JSON text) from a specific
		// SOURCE view. Replies sent via the no-target SendToWeb route back to it.
		// Malformed or non-whitelisted input is rejected and logged, never fatal.
		void HandleWebMessage(std::string_view a_viewId, std::string_view a_json);

		// Native -> web: send { type, payload }. The no-target overload sends to
		// the view whose message is currently being handled — this is what
		// command handlers use to reply. The targeted overload names the view.
		void SendToWeb(std::string_view a_type, const nlohmann::json& a_payload);
		void SendToWeb(std::string_view a_viewId, std::string_view a_type, const nlohmann::json& a_payload);

		// Native -> web handshake announcing the runtime to one view.
		void SendRuntimeReady(std::string_view a_viewId);

		// The source view of the message currently being handled (empty when
		// none is in flight). Lets a command default to its caller — e.g. a view
		// hiding itself without having to know its own id.
		[[nodiscard]] std::string_view CurrentSource() const { return _currentSource; }

	private:
		void HandleUiCommand(const nlohmann::json& a_payload);

		SendFn                                          _send;
		std::unordered_map<std::string, CommandHandler> _commands;
		std::string                                     _currentSource;  // source view of the in-flight message (reply target)
	};
}
