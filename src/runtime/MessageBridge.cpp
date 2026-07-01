#include "runtime/MessageBridge.h"

#include "core/Version.h"
#include "runtime/Json.h"

namespace OSFUI
{
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

		const auto msg = Json::Parse(a_json, "web->native message");
		if (!msg || !msg->is_object()) {
			REX::WARN("MessageBridge: rejected malformed message from view '{}'", a_viewId);
			// Surface rejections to the page (ui.error) so authors see them
			// instead of a silent drop. Existing views ignore unknown types, so
			// this is backward compatible. Echoed strings are length-bounded
			// because the inbound payload is untrusted.
			SendToWeb("ui.error", { { "reason", "malformed message" } });
			return;
		}

		const auto type = Json::GetString(*msg, "type", "");
		if (type == "ui.command") {
			const auto payloadIt = msg->find("payload");
			const auto payload = (payloadIt != msg->end() && payloadIt->is_object())
			                         ? *payloadIt
			                         : Json::Value::object();
			HandleUiCommand(payload);
		} else {
			REX::WARN("MessageBridge: rejected unknown message type '{}' from view '{}'", type, a_viewId);
			SendToWeb("ui.error", { { "reason", "unknown message type" }, { "type", type.substr(0, 128) } });
		}
	}

	void MessageBridge::HandleUiCommand(const nlohmann::json& a_payload)
	{
		const auto command = Json::GetString(a_payload, "command", "");
		// Explicit registry — no generic "call native function" escape hatch.
		if (const auto it = _commands.find(command); it != _commands.end()) {
			it->second(a_payload, *this);
		} else {
			REX::WARN("MessageBridge: rejected unknown ui.command '{}'", command);
			SendToWeb("ui.error", { { "reason", "unknown command" }, { "command", command.substr(0, 128) } });
		}
	}

	void MessageBridge::SendToWeb(std::string_view a_type, const nlohmann::json& a_payload)
	{
		// Reply to the view whose message is currently being handled.
		SendToWeb(_currentSource, a_type, a_payload);
	}

	void MessageBridge::SendToWeb(std::string_view a_viewId, std::string_view a_type, const nlohmann::json& a_payload)
	{
		if (!_send || a_viewId.empty()) {
			return;
		}
		const Json::Value msg = {
			{ "type", a_type },
			{ "payload", a_payload },
		};
		_send(a_viewId, msg.dump());
	}

	void MessageBridge::SendRuntimeReady(std::string_view a_viewId)
	{
		// `bridgeVersion` lets a view detect the protocol it's talking to and
		// degrade/refuse on a mismatch (see docs/authoring-views.md). `version`
		// stays the plugin version; the two are intentionally separate.
		SendToWeb(a_viewId, "runtime.ready", {
			{ "game", "Starfield" },
			{ "plugin", kPluginName },
			{ "version", kPluginVersion },
			{ "bridgeVersion", kBridgeProtocolVersion },
		});
	}
}
