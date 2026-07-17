#include "runtime/MessageBridge.h"

#include "core/Version.h"
#include "runtime/Json.h"

namespace OSFUI
{
	namespace
	{
		// requestIds are caller-chosen opaque strings, echoed back verbatim.
		// Bounded because the inbound payload is untrusted; over-long or
		// non-string ids are treated as absent (fire-and-forget), not truncated
		// — a silently-shortened id would never correlate.
		constexpr std::size_t kMaxRequestIdLength = 64;

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
	}

	MessageBridge::MessageBridge(SendFn a_send) :
		_send(std::move(a_send))
	{}

	void MessageBridge::RegisterCommand(std::string a_command, CommandHandler a_handler)
	{
		_commands[std::move(a_command)] = std::move(a_handler);
	}

	void MessageBridge::UnregisterCommand(std::string_view a_command)
	{
		_commands.erase(std::string(a_command));
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
			REX::WARN("MessageBridge: rejected malformed message from view '{}'", a_viewId);
			// Surface rejections to the page (ui.error) so authors see them
			// instead of a silent drop. Existing views ignore unknown types, so
			// this is backward compatible. Echoed strings are length-bounded
			// because the inbound payload is untrusted. No requestId echo — an
			// unparseable message has no readable requestId.
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
			REX::WARN("MessageBridge: rejected unknown message type '{}' from view '{}'", type, a_viewId);
			SendErrorToWeb("unknown-message-type", "unknown message type", { { "type", type.substr(0, 128) } });
		}
		_currentRequestId.clear();
		_currentCommand.clear();
		_replied = false;
	}

	void MessageBridge::HandleUiCommand(const nlohmann::json& a_payload)
	{
		const auto command = Json::GetString(a_payload, "command", "");
		_currentCommand = command.substr(0, 128);
		// Explicit registry — no generic "call native function" escape hatch.
		if (const auto it = _commands.find(command); it != _commands.end()) {
			it->second(a_payload, *this);
			// Envelope guarantee (item 5): a request-carrying command always
			// settles. A handler that replied (or deferred) already carried the
			// id; otherwise acknowledge success — verb commands (close,
			// menu.open, ...) have no reply type of their own.
			if (!_currentRequestId.empty() && !_replied) {
				SendToWeb("ui.result", { { "ok", true }, { "command", _currentCommand } });
			}
		} else {
			// Pages often retry unregistered commands (polling); warn once per
			// command name so the log records the fact without flooding. The
			// ui.error reply still goes back every time — the page needs it.
			if (_warnedUnknownCommands.insert(command).second) {
				REX::WARN("MessageBridge: rejected unknown ui.command '{}' (further rejections of this command are not logged)", command);
			}
			SendErrorToWeb("unknown-command", "unknown command", { { "command", command.substr(0, 128) } });
		}
	}

	void MessageBridge::SendErrorToWeb(std::string_view a_code, std::string_view a_message, const nlohmann::json& a_extra)
	{
		// ui.error shape (item 5): machine `code` (stable enum string) + human
		// `message` + echo fields. (A pre-1.0 `reason` duplicate of message
		// was removed at 1.0, before first release.)
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
			// Fire-and-forget caller: outcomes stay silent, exactly as before
			// the envelope existed (the handler's WARN log still records it).
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
		_send(a_viewId, EncodeMessage(a_type, a_payload, a_requestId));
	}

	void MessageBridge::SendJsonToWeb(std::string_view a_viewId, std::string_view a_type, std::string_view a_payloadJson)
	{
		if (!_send || a_viewId.empty()) {
			return;
		}
		_send(a_viewId, EncodeJsonMessage(a_type, a_payloadJson, {}));
	}

	void MessageBridge::SendJsonToWeb(const std::unordered_set<std::string>& a_viewIds, std::string_view a_type, std::string_view a_payloadJson)
	{
		if (!_send || a_viewIds.empty()) {
			return;
		}
		const auto message = EncodeJsonMessage(a_type, a_payloadJson, {});
		for (const auto& id : a_viewIds) {
			if (!id.empty()) {
				_send(id, message);
			}
		}
	}

	void MessageBridge::SendToWeb(const std::unordered_set<std::string>& a_viewIds, std::string_view a_type, const nlohmann::json& a_payload)
	{
		SendJsonToWeb(a_viewIds, a_type, a_payload.dump());
	}

	std::string MessageBridge::EncodeMessage(std::string_view a_type, const nlohmann::json& a_payload, std::string_view a_requestId)
	{
		// Serialize the payload directly into the envelope instead of first
		// deep-copying it into a temporary json object. Keep nlohmann's normal
		// object-key order (payload, requestId, type) for stable wire output.
		return EncodeJsonMessage(a_type, a_payload.dump(), a_requestId);
	}

	std::string MessageBridge::EncodeJsonMessage(std::string_view a_type, std::string_view a_payloadJson, std::string_view a_requestId)
	{
		const auto type = nlohmann::json(std::string(a_type)).dump();
		const auto requestId = a_requestId.empty()
		                           ? std::string{}
		                           : nlohmann::json(std::string(a_requestId)).dump();

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
		// `version` is the running plugin version — the reference point for
		// every advisory `targetVersion` (view manifests, settings schemas)
		// and the number a view compares against when it needs a newer-host
		// check. `bridgeVersion` is the protocol version, informational.
		SendToWeb(a_viewId, "runtime.ready", {
			{ "game", "Starfield" },
			{ "plugin", kPluginName },
			{ "version", kPluginVersion },
			{ "bridgeVersion", kBridgeProtocolVersion },
		});
	}
}
