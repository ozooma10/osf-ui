#include "runtime/MessageBridge.h"

#include "core/StringUtil.h"
#include "core/Version.h"
#include "runtime/Json.h"

namespace OSFUI
{
	namespace
	{
		// requestIds are caller-chosen opaque strings, echoed back verbatim.
		// Bounded because the inbound payload is untrusted. Over-long or
		// non-string ids are treated as absent (fire-and-forget) rather than
		// truncated — a shortened id would never correlate.
		constexpr std::size_t kMaxRequestIdLength = 64;

		// Message type / command names are view-supplied and get echoed back inside
		// ui.error and ui.result payloads. Bound them on a codepoint boundary: they
		// are dumped by SendToWeb, and dump() throws type_error.316 on a split UTF-8
		// sequence — the caller here is the web-message callback, which runs with no
		// handler between it and std::terminate.
		constexpr std::size_t kMaxEchoedNameLength = 128;

		[[nodiscard]] std::string BoundedEcho(std::string_view a_s)
		{
			return std::string{ a_s.substr(0, StringUtil::Utf8TruncateLen(a_s, kMaxEchoedNameLength)) };
		}

		std::string ExtractRequestId(const nlohmann::json& a_msg)
		{
			const auto it = a_msg.find("requestId");
			if (it == a_msg.end() || !it->is_string()) {
				return {};
			}
			const auto& id = it->get_ref<const std::string&>();
			if (id.empty() || id.size() > kMaxRequestIdLength) {
				REX::WARN("MessageBridge: ignoring requestId (must be a non-empty string of at most {} chars)", kMaxRequestIdLength);
				return {};
			}
			return id;
		}

		// Outbound trace allowlist for UNSOLICITED pushes only: the boot
		// handshake plus errors. Replies to an in-flight command never reach
		// this — they fold into that command's own trace line. Deliberately not
		// every type: ui.gamepad and friends push far too often to log.
		bool IsTracedOutbound(std::string_view a_type)
		{
			return a_type == "runtime.ready" || a_type == "settings.data" ||
				a_type == "views.data" || a_type == "diagnostics.data" ||
				a_type == "i18n.data" || a_type == "ui.error";
		}
	}

	MessageBridge::MessageBridge(SendFn a_send) :
		_send(std::move(a_send))
	{}

	void MessageBridge::RegisterCommand(std::string a_command, CommandHandler a_handler)
	{
		if (_requests.contains(a_command)) {
			REX::WARN("MessageBridge: [content] refused command '{}' — already registered as a request", a_command);
			return;
		}
		_commands[std::move(a_command)] = std::move(a_handler);
	}

	bool MessageBridge::RegisterRequest(std::string a_command, RequestHandler a_handler)
	{
		if (_commands.contains(a_command)) {
			REX::WARN("MessageBridge: [content] refused request '{}' — name already registered as a command", a_command);
			return false;
		}
		// BridgeApi owns public first-wins policy. Re-application here replaces
		// the internal trampoline after an explicit unregister/re-register.
		_requests[std::move(a_command)] = std::move(a_handler);
		return true;
	}

	void MessageBridge::UnregisterCommand(std::string_view a_command)
	{
		_commands.erase(std::string(a_command));
	}

	void MessageBridge::UnregisterRequest(std::string_view a_command)
	{
		_requests.erase(std::string(a_command));
	}

	void MessageBridge::HandleWebMessage(std::string_view a_viewId, std::string_view a_json)
	{
		// Remember the source so handler replies (and ui.error) route back to it.
		_currentSource = std::string(a_viewId);
		_currentRequestId.clear();
		_currentCommand.clear();
		_replied = false;

		const auto msg = Json::Parse(a_json, "web->native message");
		if (!msg || !msg->is_object()) {
			REX::WARN("MessageBridge: [content] rejected malformed message from view '{}'", a_viewId);
			// Surface rejections as ui.error rather than dropping silently;
			// existing views ignore unknown types, so this stays backward
			// compatible. No requestId echo — an unparseable message has none.
			SendErrorToWeb("malformed-message", "malformed message", {});
			return;
		}

		_currentRequestId = ExtractRequestId(*msg);
		const auto type = Json::GetString(*msg, "type", "");
		if (type == "ui.command") {
			const auto payloadIt = msg->find("payload");
			const auto payload = (payloadIt != msg->end() && payloadIt->is_object())
			                         ? *payloadIt
			                         : Json::Value::object();
			HandleUiCommand(payload);
		} else {
			REX::WARN("MessageBridge: [content] rejected unknown message type '{}' from view '{}'", type, a_viewId);
			SendErrorToWeb("unknown-message-type", "unknown message type", { { "type", BoundedEcho(type) } });
		}
		_currentRequestId.clear();
		_currentCommand.clear();
		_replied = false;
	}

	void MessageBridge::HandleUiCommand(const nlohmann::json& a_payload)
	{
		const auto command = Json::GetString(a_payload, "command", "");
		_currentCommand = BoundedEcho(command);
		// One DEBUG line per command, emitted on completion with what went back
		// (see NoteTracedReply). A view that renders its chrome but no data is
		// either not asking or not being answered, and nothing else in the log
		// distinguishes those two — but tracing both legs separately spent two
		// lines on every healthy request/response pair, which is the bulk of
		// bridge traffic. The folded line answers the same question in one.
		_inCommand = true;
		_traceReplies.clear();
		// Explicit registry — no generic "call native function" escape hatch.
		if (const auto it = _commands.find(command); it != _commands.end()) {
			it->second(a_payload, *this);
			// Envelope guarantee: a request-carrying command always settles. A
			// handler that replied (or deferred) already carried the id;
			// otherwise ack success — verb commands (close, menu.open, ...)
			// have no reply type of their own.
			if (!_currentRequestId.empty() && !_replied) {
				SendToWeb("ui.result", { { "ok", true }, { "command", _currentCommand } });
			}
		} else if (const auto requestIt = _requests.find(command); requestIt != _requests.end()) {
			// Request handlers own settlement; callback return is not an answer.
			requestIt->second(a_payload, *this);
			_replied = true;
		} else {
			// Pages retry unregistered commands (polling), so warn once per
			// command name to avoid flooding the log. The ui.error reply still
			// goes back every time — the page needs it.
			// Cap the dedupe set so a page spamming distinct bogus command names
			// can't grow it without bound; key on the bounded (truncated) string.
			constexpr std::size_t kMaxWarnedCommands = 512;
			const std::string warnKey{ BoundedEcho(command) };
			if (_warnedUnknownCommands.size() < kMaxWarnedCommands &&
				_warnedUnknownCommands.insert(warnKey).second) {
				REX::WARN("MessageBridge: [content] rejected unknown ui.command '{}' (further rejections of this command are not logged)", warnKey);
			}
			SendErrorToWeb("unknown-command", "unknown command", { { "command", BoundedEcho(command) } });
		}

		_inCommand = false;
		// "(no reply)" is not by itself a fault — fire-and-forget verb commands
		// (close, menu.open, ...) legitimately answer nothing. It is the signal
		// to look at when a data request came in and the view stayed empty.
		REX::DEBUG("MessageBridge: '{}' from view '{}' -> {}", _currentCommand, _currentSource,
			_traceReplies.empty() ? std::string_view{ "(no reply)" } : std::string_view{ _traceReplies });
	}

	bool MessageBridge::NoteTracedReply(std::string_view a_viewId, std::string_view a_type)
	{
		if (!_inCommand || a_viewId != _currentSource) {
			return false;  // unsolicited push: the caller logs it as before
		}
		// Bounded: a handler answering in a loop must not grow this without
		// limit. Past the cap the line ends in an ellipsis rather than growing.
		constexpr std::size_t kMaxTraceLength = 160;
		if (_traceReplies.size() >= kMaxTraceLength) {
			if (!_traceReplies.ends_with("...")) {
				_traceReplies += ", ...";
			}
			return true;
		}
		if (!_traceReplies.empty()) {
			_traceReplies += ", ";
		}
		_traceReplies.append(a_type);
		return true;
	}

	void MessageBridge::SendErrorToWeb(std::string_view a_code, std::string_view a_message, const nlohmann::json& a_extra)
	{
		// ui.error shape: machine `code` (stable enum string) + human `message`
		// + echo fields. A pre-1.0 `reason` duplicate of message was removed
		// at 1.0, before first release.
		nlohmann::json payload = {
			{ "code", a_code },
			{ "message", a_message },
		};
		for (const auto& [key, value] : a_extra.items()) {
			payload[key] = value;
		}
		SendToWeb("ui.error", payload);
	}

	void MessageBridge::SendResult(bool a_ok, std::string_view a_code, std::string_view a_message)
	{
		if (_currentRequestId.empty()) {
			// Fire-and-forget caller: outcomes stay silent, as before the
			// envelope existed (the handler's WARN log still records it).
			_replied = true;  // suppress the auto-ack either way
			return;
		}
		nlohmann::json payload = { { "ok", a_ok } };
		if (!_currentCommand.empty()) {
			payload["command"] = _currentCommand;
		}
		if (!a_code.empty()) {
			payload["code"] = a_code;
		}
		if (!a_message.empty()) {
			payload["message"] = a_message;
		}
		SendToWeb("ui.result", payload);
	}

	void MessageBridge::SendToWeb(std::string_view a_type, const nlohmann::json& a_payload)
	{
		// Reply to the view whose message is currently being handled, echoing
		// the in-flight requestId (if any) so the caller can correlate.
		SendToWeb(_currentSource, a_type, a_payload, _currentRequestId);
		_replied = true;
	}

	void MessageBridge::SendToWeb(std::string_view a_viewId, std::string_view a_type, const nlohmann::json& a_payload)
	{
		SendToWeb(a_viewId, a_type, a_payload, {});
	}

	void MessageBridge::SendToWeb(std::string_view a_viewId, std::string_view a_type, const nlohmann::json& a_payload, std::string_view a_requestId)
	{
		if (!_send || a_viewId.empty()) {
			return;
		}
		if (!NoteTracedReply(a_viewId, a_type) && IsTracedOutbound(a_type)) {
			REX::DEBUG("MessageBridge: native->web '{}' to view '{}'", a_type, a_viewId);
		}
		_send(a_viewId, EncodeMessage(a_type, a_payload, a_requestId));
	}

	void MessageBridge::SendJsonToWeb(std::string_view a_viewId, std::string_view a_type, std::string_view a_payloadJson)
	{
		if (!_send || a_viewId.empty()) {
			return;
		}
		if (!NoteTracedReply(a_viewId, a_type) && IsTracedOutbound(a_type)) {
			REX::DEBUG("MessageBridge: native->web '{}' to view '{}'", a_type, a_viewId);
		}
		_send(a_viewId, EncodeJsonMessage(a_type, a_payloadJson, {}));
	}

	void MessageBridge::SendJsonToWeb(const std::unordered_set<std::string>& a_viewIds, std::string_view a_type, std::string_view a_payloadJson)
	{
		if (!_send || a_viewIds.empty()) {
			return;
		}
		const auto message = EncodeJsonMessage(a_type, a_payloadJson, {});
		const bool trace = IsTracedOutbound(a_type);
		for (const auto& id : a_viewIds) {
			if (!id.empty()) {
				// A fan-out triggered by a command still folds the caller's own
				// copy into that command's line; the other views log normally.
				if (!NoteTracedReply(id, a_type) && trace) {
					REX::DEBUG("MessageBridge: native->web '{}' to view '{}'", a_type, id);
				}
				_send(id, message);
			}
		}
	}

	void MessageBridge::SendToWeb(const std::unordered_set<std::string>& a_viewIds, std::string_view a_type, const nlohmann::json& a_payload)
	{
		SendJsonToWeb(a_viewIds, a_type, Json::Dump(a_payload));
	}

	std::string MessageBridge::EncodeMessage(std::string_view a_type, const nlohmann::json& a_payload, std::string_view a_requestId)
	{
		// Serialize the payload straight into the envelope rather than
		// deep-copying it into a temporary json object. Key order stays
		// nlohmann's (payload, requestId, type) for stable wire output.
		return EncodeJsonMessage(a_type, Json::Dump(a_payload), a_requestId);
	}

	std::string MessageBridge::EncodeJsonMessage(std::string_view a_type, std::string_view a_payloadJson, std::string_view a_requestId)
	{
		const auto type = Json::Dump(nlohmann::json(std::string(a_type)));
		const auto requestId = a_requestId.empty()
		                           ? std::string{}
		                           : Json::Dump(nlohmann::json(std::string(a_requestId)));

		std::string message;
		message.reserve(a_payloadJson.size() + type.size() + requestId.size() + 48);
		message += R"({"payload":)";
		message += a_payloadJson;
		if (!requestId.empty()) {
			message += R"(,"requestId":)";
			message += requestId;
		}
		message += R"(,"type":)";
		message += type;
		message += '}';
		return message;
	}

	void MessageBridge::SendRuntimeReady(std::string_view a_viewId)
	{
		// `version` is the running plugin version — the reference point every
		// advisory `targetVersion` (view manifests, settings schemas) and any
		// view-side newer-host check compares against. `bridgeVersion` is the
		// protocol version, informational.
		SendToWeb(a_viewId, "runtime.ready", {
			{ "game", "Starfield" },
			{ "plugin", kPluginName },
			{ "version", kPluginVersion },
			{ "bridgeVersion", kBridgeProtocolVersion },
		});
	}
}
