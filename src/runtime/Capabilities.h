#pragma once

namespace OSFUI::Caps
{
	// Named host capabilities (docs/api-freeze-plan.md items 2 + 6).
	//
	// One vocabulary serves two consumers: a settings schema's `requires`
	// array (evaluated at registration — unmet ⇒ the mod registers as a stub
	// card, item 2) and the `capabilities` field of the `runtime.ready`
	// handshake (item 6, emitted by MessageBridge::SendRuntimeReady) that JS
	// views gate on via `osfui.has()`.
	//
	// The list is APPEND-ONLY: a capability, once shipped, is never removed
	// or renamed. Naming: command-namespace names for surfaces
	// ("settings", "game.calendar"), `type:<t>` for setting value types,
	// `schema:<feature>` for schema-level mechanisms.
	inline constexpr std::string_view kList[] = {
		"settings",             // settings.get/set/reset + schema registry
		"settings.captureKey",  // native key-rebind capture
		"views",                // views.get catalog + subscription
		"i18n",                 // i18n.get catalog + live locale/catalog pushes
		"game.calendar",        // game.get calendar provider
		"gamepad",              // engine-routed gamepad events (ui.gamepad)
		"schema:requires",      // this gate itself (hosts older than it ignore the field)
		"request-id",           // item-5 envelope: requestId echo + ui.result
		"type:bool",
		"type:int",
		"type:float",
		"type:enum",
		"type:string",
		"type:key",
		"type:flags",
		"settings.loadErrors",  // settings.data carries top-level `loadErrors` (protocol 1.1)
	};

	inline bool Has(std::string_view a_name)
	{
		for (const auto cap : kList) {
			if (cap == a_name) {
				return true;
			}
		}
		return false;
	}
}
