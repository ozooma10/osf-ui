#include "runtime/ControlMapPolicy.h"

#include <algorithm>

namespace OSFUI::ControlMapPolicy
{
	namespace
	{
		constexpr GameplayModeMask kOnFoot = ModeBit(GameplayMode::OnFoot);
		constexpr GameplayModeMask kShip = ModeBit(GameplayMode::Ship);
		constexpr GameplayModeMask kVehicle = ModeBit(GameplayMode::Vehicle);
		constexpr GameplayModeMask kZeroG = ModeBit(GameplayMode::ZeroG);

		bool IsMenuFamily(std::uint8_t a_id, std::string_view a_name)
		{
			if (a_name.starts_with("BasicMenuNav") || a_name.starts_with("DataMenu") ||
				a_name.starts_with("StarMap") || a_name.starts_with("ShipBuilder_")) {
				return true;
			}
			return a_id == 0x18 ||  // Terminal
			       a_id == 0x19 ||  // PhotoMode
			       a_id == 0x2B;    // SurfaceMap
		}
	}

	EngineInputContextPolicy Classify(std::uint8_t a_engineInputContextId,
		std::string_view a_engineInputContextName)
	{
		switch (a_engineInputContextId) {
			case 0x00: return { Classification::Core, kOnFoot | kShip, kVehicle | kZeroG };
			case 0x21: return { Classification::Core, kShip, 0 };
			case 0x16: // Workshop
			case 0x1B: // Scope
				return { Classification::Special, 0, kOnFoot };
			case 0x20: return { Classification::Special, 0, kZeroG };
			case 0x22:
			case 0x26:
			case 0x27:
			case 0x4D:
				return { Classification::Special, 0, kShip };
			case 0x49: return { Classification::Special, 0, kVehicle };
			default: break;
		}
		if (IsMenuFamily(a_engineInputContextId, a_engineInputContextName)) {
			return { Classification::Menu, 0, 0 };
		}
		return {};
	}

	std::string_view ClassificationName(Classification a_classification)
	{
		switch (a_classification) {
			case Classification::Core: return "core";
			case Classification::Special: return "special";
			case Classification::Menu: return "menu";
			case Classification::Unknown: return "unknown";
		}
		return "unknown";
	}

	bool IsDefiniteShipEngineInputContext(std::uint8_t a_engineInputContextId)
	{
		// ShipHUD is the only v1 engine input context proved as ship-definite.
		return a_engineInputContextId == 0x21;
	}

	std::optional<GameplayMode> DeriveMode(std::span<const std::uint8_t> a_activeEngineInputContexts)
	{
		const auto has = [&](std::uint8_t a_id) {
			return std::ranges::find(a_activeEngineInputContexts, a_id) != a_activeEngineInputContexts.end();
		};
		// Proven semantic precedence: MainGameplay can remain active underneath
		// each of these more specific modes.
		if (has(0x49)) return GameplayMode::Vehicle;
		if (std::ranges::any_of(a_activeEngineInputContexts, IsDefiniteShipEngineInputContext)) {
			return GameplayMode::Ship;
		}
		if (has(0x20)) return GameplayMode::ZeroG;
		if (has(0x00)) return GameplayMode::OnFoot;
		return std::nullopt;
	}
}
