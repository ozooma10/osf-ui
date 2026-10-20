#pragma once

#include <algorithm>
#include <cstdint>

namespace OSFUI
{
	struct AbsoluteMouseMapping
	{
		float x{ 0.0f };
		float y{ 0.0f };
		bool  inside{ false };
	};

	struct CenteredFit
	{
		float x{ 0.0f };
		float y{ 0.0f };
		float scale{ 0.0f };
	};

	// Largest aspect-preserving placement of a view inside an output, centered (letterbox/pillarbox).
	[[nodiscard]] inline CenteredFit FitCentered(
		const float a_outputWidth,
		const float a_outputHeight,
		const std::uint32_t a_viewWidth,
		const std::uint32_t a_viewHeight)
	{
		if (a_outputWidth <= 0.0f || a_outputHeight <= 0.0f || a_viewWidth == 0 || a_viewHeight == 0) {
			return {};
		}
		const auto viewWidth = static_cast<float>(a_viewWidth);
		const auto viewHeight = static_cast<float>(a_viewHeight);
		const auto scale = (std::min)(a_outputWidth / viewWidth, a_outputHeight / viewHeight);
		return {
			.x = (a_outputWidth - viewWidth * scale) * 0.5f,
			.y = (a_outputHeight - viewHeight * scale) * 0.5f,
			.scale = scale,
		};
	}

	// Map the game client through the centered viewport used to display the UI target.
	[[nodiscard]] inline AbsoluteMouseMapping MapAbsoluteMouseToView(
		const int a_clientX,
		const int a_clientY,
		const int a_clientWidth,
		const int a_clientHeight,
		const std::uint32_t a_viewWidth,
		const std::uint32_t a_viewHeight)
	{
		if (a_clientWidth <= 0 || a_clientHeight <= 0 ||
			a_viewWidth == 0 || a_viewHeight == 0) {
			return {};
		}

		const auto viewWidth = static_cast<float>(a_viewWidth);
		const auto viewHeight = static_cast<float>(a_viewHeight);
		const auto fit = FitCentered(static_cast<float>(a_clientWidth), static_cast<float>(a_clientHeight), a_viewWidth, a_viewHeight);
		const auto rawX = (static_cast<float>(a_clientX) - fit.x) / fit.scale;
		const auto rawY = (static_cast<float>(a_clientY) - fit.y) / fit.scale;

		return {
			.x = std::clamp(rawX, 0.0f, viewWidth - 1.0f),
			.y = std::clamp(rawY, 0.0f, viewHeight - 1.0f),
			.inside = rawX >= 0.0f && rawX < viewWidth && rawY >= 0.0f && rawY < viewHeight,
		};
	}
}
