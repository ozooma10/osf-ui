#pragma once

#include "runtime/InputModes.h"

#include <cstdint>
#include <optional>
#include <span>
#include <string_view>

namespace OSFUI::ControlMapPolicy
{
	enum class Classification : std::uint8_t
	{
		Core,
		Special,
		Menu,
		Unknown,
	};

	struct EngineInputContextPolicy
	{
		Classification classification{ Classification::Unknown };
		GameplayModeMask definiteModes{ 0 };
		GameplayModeMask possibleModes{ 0 };
	};

	[[nodiscard]] EngineInputContextPolicy Classify(std::uint8_t a_engineInputContextId,
		std::string_view a_engineInputContextName);
	[[nodiscard]] std::string_view ClassificationName(Classification a_classification);
	[[nodiscard]] bool IsDefiniteShipEngineInputContext(std::uint8_t a_engineInputContextId);
	[[nodiscard]] std::optional<GameplayMode> DeriveMode(
		std::span<const std::uint8_t> a_activeEngineInputContexts);
}
