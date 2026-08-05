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

	struct ContextPolicy
	{
		Classification classification{ Classification::Unknown };
		GameplayModeMask definiteModes{ 0 };
		GameplayModeMask possibleModes{ 0 };
	};

	[[nodiscard]] ContextPolicy Classify(std::uint8_t a_contextId, std::string_view a_contextName);
	[[nodiscard]] std::string_view ClassificationName(Classification a_classification);
	[[nodiscard]] bool IsDefiniteShipContext(std::uint8_t a_contextId);
	[[nodiscard]] std::optional<GameplayMode> DeriveMode(std::span<const std::uint8_t> a_activeContexts);
}
