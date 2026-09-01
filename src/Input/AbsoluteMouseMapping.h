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
		const auto scale = (std::min)(
			static_cast<float>(a_clientWidth) / viewWidth,
			static_cast<float>(a_clientHeight) / viewHeight);
		const auto offsetX = (static_cast<float>(a_clientWidth) - viewWidth * scale) * 0.5f;
		const auto offsetY = (static_cast<float>(a_clientHeight) - viewHeight * scale) * 0.5f;
		const auto rawX = (static_cast<float>(a_clientX) - offsetX) / scale;
		const auto rawY = (static_cast<float>(a_clientY) - offsetY) / scale;

		return {
			.x = std::clamp(rawX, 0.0f, viewWidth - 1.0f),
			.y = std::clamp(rawY, 0.0f, viewHeight - 1.0f),
			.inside = rawX >= 0.0f && rawX < viewWidth && rawY >= 0.0f && rawY < viewHeight,
		};
	}
}
