#include "runtime/MessageBridge.h"

#include "core/Version.h"
#include "runtime/Json.h"

namespace SWUI
{
	MessageBridge::MessageBridge(Host a_host) :
		_host(std::move(a_host))
	{}

	void MessageBridge::HandleWebMessage(std::string_view a_json)
	{
		const auto msg = Json::Parse(a_json, "web->native message");
		if (!msg || !msg->is_object()) {
			REX::WARN("MessageBridge: rejected malformed message");
			return;
		}

		const auto type = Json::GetString(*msg, "type", "");
		const auto payloadIt = msg->find("payload");
		const auto payload = (payloadIt != msg->end() && payloadIt->is_object())
		                         ? *payloadIt
		                         : Json::Value::object();

		if (type == "ui.command") {
			HandleUiCommand(payload);
		} else {
			REX::WARN("MessageBridge: rejected unknown message type '{}'", type);
		}
	}

	void MessageBridge::HandleUiCommand(const nlohmann::json& a_payload)
	{
		const auto command = Json::GetString(a_payload, "command", "");

		// Explicit whitelist. Anything not listed here is rejected — do not
		// add a generic "call native function" escape hatch.
		if (command == "close") {
			REX::INFO("MessageBridge: ui.command close");
			if (_host.setVisible) {
				_host.setVisible(false);
			}
		} else if (command == "log") {
			const auto text = Json::GetString(a_payload, "text", "");
			// Untrusted content: bound the length so JS cannot flood the log.
			REX::INFO("MessageBridge: [web] {}", text.substr(0, 512));
		} else if (command == "ping") {
			if (_host.sendToWeb) {
				const Json::Value pong = {
					{ "type", "runtime.pong" },
					{ "payload", Json::Value::object() },
				};
				_host.sendToWeb(pong.dump());
			}
		} else if (command == "setVisible") {
			const auto visible = Json::GetBool(a_payload, "visible", false);
			REX::INFO("MessageBridge: ui.command setVisible({})", visible);
			if (_host.setVisible) {
				_host.setVisible(visible);
			}
		} else if (command == "settings.get") {
			// Send the schema + current values to the view.
			if (_host.getSettingsData && _host.sendToWeb) {
				const Json::Value msg = {
					{ "type", "settings.data" },
					{ "payload", Json::Value::parse(_host.getSettingsData(), nullptr, false) },
				};
				_host.sendToWeb(msg.dump());
			}
		} else if (command == "settings.set") {
			// Persist one setting. key + value are validated against the schema
			// by the store; we only forward the raw value JSON.
			const auto key = Json::GetString(a_payload, "key", "");
			const auto valueIt = a_payload.find("value");
			const bool ok = _host.setSetting && valueIt != a_payload.end() &&
				_host.setSetting(key, valueIt->dump());
			if (_host.sendToWeb) {
				const Json::Value ack = {
					{ "type", "settings.ack" },
					{ "payload", { { "key", key }, { "ok", ok } } },
				};
				_host.sendToWeb(ack.dump());
			}
		} else {
			REX::WARN("MessageBridge: rejected unknown ui.command '{}'", command);
		}
	}

	void MessageBridge::SendRuntimeReady()
	{
		if (!_host.sendToWeb) {
			return;
		}
		const Json::Value ready = {
			{ "type", "runtime.ready" },
			{ "payload", {
				{ "game", "Starfield" },
				{ "plugin", kPluginName },
				{ "version", kPluginVersion },
			} },
		};
		_host.sendToWeb(ready.dump());
	}
}
