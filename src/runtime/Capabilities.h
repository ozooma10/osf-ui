#pragma once

namespace OSFUI::Caps
{
	// Named host capabilities (docs/api-freeze-plan.md items 2 + 6).
	//
	// One vocabulary serves two consumers: a settings schema's `requires`
	// array (evaluated at registration — unmet ⇒ the mod registers as a stub
	// card, item 2) and, once item 6 lands, the `capabilities` field of the
	// `runtime.ready` handshake for JS views.
	//
	// The list is APPEND-ONLY: a capability, once shipped, is never removed
	// or renamed. Naming: command-namespace names for surfaces
	// ("settings", "game.calendar"), `type:<t>` for setting value types,
	// `schema:<feature>` for schema-level mechanisms.
	inline bool Has(std::string_view a_name)
	{
		static constexpr std::string_view kAll[] = {
			"settings",             // settings.get/set/reset + schema registry
			"settings.captureKey",  // native key-rebind capture
			"views",                // views.get catalog + subscription
			"game.calendar",        // game.get calendar provider
			"gamepad",              // engine-routed gamepad events (ui.gamepad)
			"schema:requires",      // this gate itself (hosts older than it ignore the field)
			"type:bool",
			"type:int",
			"type:float",
			"type:enum",
			"type:string",
			"type:key",
			"type:flags",
			// "request-id" lands with the item-5 envelope (protocol 0.5).
		};
		for (const auto cap : kAll) {
			if (cap == a_name) {
				return true;
			}
		}
		return false;
	}
}
