#pragma once

#include <unordered_set>  // not in pch.h

#include <nlohmann/json.hpp>

// Narrow native <-> web bridge. All traffic is JSON text messages with the
// shape { "type": string, "requestId"?: string, "payload": object }. Web ->
// native traffic is a single type, `ui.command`, dispatched by its `command`
// field against a registry of handlers. The bridge itself is FEATURE-AGNOSTIC:
// core registers platform commands (close/setVisible/...), and each IUiModule
// registers its own (e.g. settings.*). There is intentionally NO mechanism to
// call arbitrary native functions from JS. See docs/security-model.md.
//
// Request/result envelope (api-freeze-plan item 5, protocol 1.0): any
// ui.command may carry a caller-chosen `requestId` (string, <=64 chars).
// While that command is being handled, every reply sent through the no-target
// SendToWeb echoes the id top-level; if the handler produced no reply of its
// own, the bridge answers `ui.result { ok:true, command }` so the caller's
// promise always settles. Handlers report failures via SendResult (emitted
// only when a requestId was supplied — fire-and-forget stays silent).

namespace OSFUI
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

		// Remove a previously-registered command (no-op if absent). Used by the
		// native plugin API (src/api) for hot cleanup / re-sync.
		void UnregisterCommand(std::string_view a_command);

		// Entry point for web -> native messages (raw JSON text) from a specific
		// SOURCE view. Replies sent via the no-target SendToWeb route back to it.
		// Malformed or non-whitelisted input is rejected and logged, never fatal.
		void HandleWebMessage(std::string_view a_viewId, std::string_view a_json);

		// Native -> web: send { type, payload }. The no-target overload sends to
		// the view whose message is currently being handled — this is what
		// command handlers use to reply; it echoes the in-flight requestId (if
		// any) top-level and suppresses the automatic ui.result. The targeted
		// overloads name the view; the 4-arg form attaches an explicit
		// requestId — the deferred-reply path (e.g. settings.captured answers a
		// capture armed ticks earlier).
		void SendToWeb(std::string_view a_type, const nlohmann::json& a_payload);
		void SendToWeb(std::string_view a_viewId, std::string_view a_type, const nlohmann::json& a_payload);
		void SendToWeb(std::string_view a_viewId, std::string_view a_type, const nlohmann::json& a_payload, std::string_view a_requestId);
		// Internal prevalidated-text path used by the C ABI queue: its public
		// SendToWeb entry point already parsed this payload synchronously.
		void SendJsonToWeb(std::string_view a_viewId, std::string_view a_type, std::string_view a_payloadJson);
		void SendJsonToWeb(const std::unordered_set<std::string>& a_viewIds, std::string_view a_type, std::string_view a_payloadJson);
		// Fan out one identical envelope. The JSON text is encoded once, then
		// handed to every target transport; useful for settings/catalog pushes.
		void SendToWeb(const std::unordered_set<std::string>& a_viewIds, std::string_view a_type, const nlohmann::json& a_payload);

		// Report the in-flight command's outcome as `ui.result { ok, command,
		// code?, message? }` — but ONLY when the caller supplied a requestId
		// (fire-and-forget callers keep today's silence; the WARN log is the
		// handler's job). Codes are stable machine strings ("unknown-view",
		// "capture-busy", ...); a_message is the human sentence.
		void SendResult(bool a_ok, std::string_view a_code = {}, std::string_view a_message = {});

		// Mark the in-flight request as answered WITHOUT sending anything now —
		// the deferred-reply handshake. The handler stashes CurrentRequestId()
		// and later echoes it via the 4-arg SendToWeb.
		void DeferResult() { _replied = true; }

		// Native -> web handshake announcing the runtime to one view.
		void SendRuntimeReady(std::string_view a_viewId);

		// The source view of the message currently being handled (empty when
		// none is in flight). Lets a command default to its caller — e.g. a view
		// hiding itself without having to know its own id.
		[[nodiscard]] std::string_view CurrentSource() const { return _currentSource; }

		// The requestId of the message currently being handled ("" = none —
		// fire-and-forget or no message in flight).
		[[nodiscard]] std::string_view CurrentRequestId() const { return _currentRequestId; }

	private:
		[[nodiscard]] static std::string EncodeMessage(std::string_view a_type, const nlohmann::json& a_payload, std::string_view a_requestId);
		[[nodiscard]] static std::string EncodeJsonMessage(std::string_view a_type, std::string_view a_payloadJson, std::string_view a_requestId);
		void HandleUiCommand(const nlohmann::json& a_payload);
		void SendErrorToWeb(std::string_view a_code, std::string_view a_message, const nlohmann::json& a_extra);

		SendFn                                          _send;
		std::unordered_map<std::string, CommandHandler> _commands;
		std::string                                     _currentSource;     // source view of the in-flight message (reply target)
		std::string                                     _currentRequestId;  // requestId of the in-flight message ("" = none)
		std::string                                     _currentCommand;    // command of the in-flight ui.command (ui.result echo)
		bool                                            _replied{ false };  // a reply carried the requestId (suppresses auto ui.result)
		std::unordered_set<std::string>                 _warnedUnknownCommands;  // warn-once-per-command log dedupe
	};
}
