#pragma once

#include <cstdint>
#include <optional>

namespace OSFUI::ScreenFormId
{
	// Starfield compiles plugins into three tiers. Each tier spends a different
	// number of form-ID bits on the plugin slot, so a catalogue's author-time
	// local ID only composes against the tier its plugin actually loaded into.
	enum class Tier
	{
		Full,    // compile index 0x00..0xFC, 24-bit local
		Medium,  // compile index 0xFD, 8-bit slot, 16-bit local
		Small    // compile index 0xFE, 12-bit slot, 12-bit local
	};

	// Composes the runtime form ID for a loaded plugin's slot. Returns nullopt
	// when the slot or the local ID does not fit the tier, which is an authoring
	// error rather than an absent plugin: masking would silently bind a
	// different form.
	[[nodiscard]] std::optional<std::uint32_t> Compose(Tier tier, std::uint32_t slot, std::uint32_t local);
}
