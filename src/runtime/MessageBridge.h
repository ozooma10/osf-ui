#pragma once

#include <unordered_set>  // not in pch.h

#include <nlohmann/json.hpp>

// Narrow native <-> web bridge. All traffic is JSON text messages shaped
// { "type": string, "requestId"?: string, "payload": object }. Web -> native
// traffic is a single type, `ui.command`, dispatched by its `command` field
// against a registry of handlers. The bridge is feature-agnostic: core
// registers platform commands (close/setVisible/...), each IUiModule registers
// its own (e.g. settings.*). There is no mechanism to call arbitrary native
// functions from JS. See docs/security-model.md.
//
// Request/result envelope (api-freeze-plan item 5, protocol 1.0): any
// ui.command may carry a caller-chosen `requestId` (string, <=64 chars). While
// that command is being handled, every reply sent through the no-target
// SendToWeb echoes the id top-level; if the handler produced no reply of its
// own, the bridge answers `ui.result { ok:true, command }` so the caller's
// promise always settles. Handlers report failures via SendResult, emitted only
// when a requestId was supplied — fire-and-forget stays silent.

namespace OSFUI
{
	class MessageBridge
	{
	public:
		// Transport for native -> web text: deliver `a_json` to view `a_viewId`.
		using SendFn = std::function<void(std::string_view a_viewId, std::string_view a_json)>;

		// Handler for one `ui.command` command: the payload plus the bridge to
		// reply through. Registered by core/modules.
		using CommandHandler = std::function<void(const nlohmann::json& a_payload, MessageBridge& a_bridge)>;

		explicit MessageBridge(SendFn a_send);

		// Register (or replace) the handler for an exact command string, e.g.
		// "settings.get". Unknown commands are rejected and logged.
		void RegisterCommand(std::string a_command, CommandHandler a_handler);

		// No-op if absent. Used by the native plugin API (src/api) for hot
		// cleanup / re-sync.
		void UnregisterCommand(std::string_view a_command);

		// Entry point for web -> native messages (raw JSON text) from a given
		// source view; replies via the no-target SendToWeb route back to it.
		// Malformed or non-whitelisted input is rejected and logged, never fatal.
		void HandleWebMessage(std::string_view a_viewId, std::string_view a_json);

		// Native -> web: send { type, payload }. The no-target overload sends to
		// the view whose message is currently being handled — what command
		// handlers reply through; it echoes the in-flight requestId (if any)
		// top-level and suppresses the automatic ui.result. The targeted
		// overloads name the view; the 4-arg form attaches an explicit requestId
		// for the deferred-reply path (e.g. settings.captured answering a
		// capture armed ticks earlier).
		void SendToWeb(std::string_view a_type, const nlohmann::json& a_payload);
		void SendToWeb(std::string_view a_viewId, std::string_view a_type, const nlohmann::json& a_payload);
		void SendToWeb(std::string_view a_viewId, std::string_view a_type, const nlohmann::json& a_payload, std::string_view a_requestId);
		// Prevalidated-text path for the C ABI queue: its public SendToWeb entry
		// point already parsed this payload synchronously.
		void SendJsonToWeb(std::string_view a_viewId, std::string_view a_type, std::string_view a_payloadJson);
		void SendJsonToWeb(const std::unordered_set<std::string>& a_viewIds, std::string_view a_type, std::string_view a_payloadJson);
		// Fan out one identical envelope: encoded once, handed to every target
		// transport. Used for settings/catalog pushes.
		void SendToWeb(const std::unordered_set<std::string>& a_viewIds, std::string_view a_type, const nlohmann::json& a_payload);

		// Report the in-flight command's outcome as `ui.result { ok, command,
		// code?, message? }`, only when the caller supplied a requestId —
		// fire-and-forget callers stay silent, and logging is the handler's job.
		// Codes are stable machine strings ("unknown-view", "capture-busy", ...);
		// a_message is the human sentence.
		void SendResult(bool a_ok, std::string_view a_code = {}, std::string_view a_message = {});

		// Deferred-reply handshake: mark the in-flight request answered without
		// sending anything now. The handler stashes CurrentRequestId() and later
		// echoes it via the 4-arg SendToWeb.
		void DeferResult() { _replied = true; }

		// Native -> web handshake announcing the runtime to one view.
		void SendRuntimeReady(std::string_view a_viewId);

		// Source view of the in-flight message (empty when none). Lets a command
		// default to its caller, e.g. a view hiding itself without knowing its
		// own id.
		[[nodiscard]] std::string_view CurrentSource() const { return _currentSource; }

		// requestId of the in-flight message ("" = fire-and-forget, or nothing
		// in flight).
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
