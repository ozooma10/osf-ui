#pragma once

#include <nlohmann/json_fwd.hpp>

// Narrow native <-> web bridge. All traffic is JSON text messages with the
// shape { "type": string, "payload": object }. Web -> native commands are
// dispatched against an explicit whitelist; everything else is rejected and
// logged. There is intentionally NO mechanism to call arbitrary native
// functions from JS. See docs/security-model.md.

namespace SWUI
{
	class MessageBridge
	{
	public:
		// Host hooks the bridge needs. Kept as narrow function objects so the
		// bridge never holds a reference to the whole Runtime.
		struct Host
		{
			std::function<void(bool)>             setVisible;  // drives ui.command "setVisible" / "close"
			std::function<void(std::string_view)> sendToWeb;   // outbound JSON (-> renderer SendMessageToWeb)
		};

		explicit MessageBridge(Host a_host);

		// Entry point for web -> native messages (raw JSON text from the JS
		// side). Malformed or non-whitelisted input is rejected and logged,
		// never fatal.
		void HandleWebMessage(std::string_view a_json);

		// Native -> web notifications.
		void SendRuntimeReady();

	private:
		void HandleUiCommand(const nlohmann::json& a_payload);

		Host _host;
	};
}
