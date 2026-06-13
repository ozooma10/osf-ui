#include "runtime/MessageBridge.h"

#include "core/Version.h"
#include "runtime/Json.h"

namespace SWUI
{
	MessageBridge::MessageBridge(SendFn a_send) :
		_send(std::move(a_send))
	{}

	void MessageBridge::RegisterCommand(std::string a_command, CommandHandler a_handler)
	{
		_commands[std::move(a_command)] = std::move(a_handler);
	}

	void MessageBridge::HandleWebMessage(std::string_view a_json)
	{
		const auto msg = Json::Parse(a_json, "web->native message");
		if (!msg || !msg->is_object()) {
			REX::WARN("MessageBridge: rejected malformed message");
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
			REX::WARN("MessageBridge: rejected unknown message type '{}'", type);
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
		}
	}

	void MessageBridge::SendToWeb(std::string_view a_type, const nlohmann::json& a_payload)
	{
		if (!_send) {
			return;
		}
		const Json::Value msg = {
			{ "type", a_type },
			{ "payload", a_payload },
		};
		_send(msg.dump());
	}

	void MessageBridge::SendRuntimeReady()
	{
		SendToWeb("runtime.ready", {
			{ "game", "Starfield" },
			{ "plugin", kPluginName },
			{ "version", kPluginVersion },
		});
	}
}
