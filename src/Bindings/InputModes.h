#pragma once

#include <array>
#include <cstdint>
#include <optional>
#include <string_view>

namespace OSFUI
{
	enum class GameplayMode : std::uint8_t
	{
		OnFoot = 1u << 0,
		Ship = 1u << 1,
		Vehicle = 1u << 2,
		ZeroG = 1u << 3,
	};

	using GameplayModeMask = std::uint8_t;

	inline constexpr GameplayModeMask kAllGameplayModes =
		static_cast<GameplayModeMask>(GameplayMode::OnFoot) |
		static_cast<GameplayModeMask>(GameplayMode::Ship) |
		static_cast<GameplayModeMask>(GameplayMode::Vehicle) |
		static_cast<GameplayModeMask>(GameplayMode::ZeroG);

	[[nodiscard]] constexpr GameplayModeMask ModeBit(GameplayMode a_mode)
	{
		return static_cast<GameplayModeMask>(a_mode);
	}

	[[nodiscard]] constexpr bool ModesOverlap(GameplayModeMask a_left, GameplayModeMask a_right)
	{
		return (a_left & a_right) != 0;
	}

	[[nodiscard]] inline std::optional<GameplayMode> GameplayModeFromName(std::string_view a_name)
	{
		if (a_name == "onFoot") return GameplayMode::OnFoot;
		if (a_name == "ship") return GameplayMode::Ship;
		if (a_name == "vehicle") return GameplayMode::Vehicle;
		if (a_name == "zeroG") return GameplayMode::ZeroG;
		return std::nullopt;
	}

	[[nodiscard]] inline std::string_view GameplayModeName(GameplayMode a_mode)
	{
		switch (a_mode) {
			case GameplayMode::OnFoot: return "onFoot";
			case GameplayMode::Ship: return "ship";
			case GameplayMode::Vehicle: return "vehicle";
			case GameplayMode::ZeroG: return "zeroG";
		}
		return {};
	}
}
