#pragma once

#include <cstdint>

namespace OSFUI
{
	struct ViewSize
	{
		std::uint32_t width{ 0 };
		std::uint32_t height{ 0 };
	};

	[[nodiscard]] constexpr std::uint64_t PackViewSize(const ViewSize a_size)
	{
		return (static_cast<std::uint64_t>(a_size.width) << 32) | a_size.height;
	}

	[[nodiscard]] constexpr ViewSize UnpackViewSize(const std::uint64_t a_size)
	{
		return {
			.width = static_cast<std::uint32_t>(a_size >> 32),
			.height = static_cast<std::uint32_t>(a_size),
		};
	}

	// Chargen's Scaleform composite contains its source in a centered 16:9
	// viewport. Keep capture output-sized and lay out the browser document in
	// this viewport so the shared ring never resizes on menu transitions.
	[[nodiscard]] constexpr ViewSize ViewSizeForOutput(
		const ViewSize a_output, const bool a_fixed16By9)
	{
		if (!a_fixed16By9 || a_output.width == 0 || a_output.height == 0) {
			return a_output;
		}

		constexpr std::uint64_t widthRatio = 16;
		constexpr std::uint64_t heightRatio = 9;
		const auto outputCross =
			static_cast<std::uint64_t>(a_output.width) * heightRatio;
		const auto fixedCross =
			static_cast<std::uint64_t>(a_output.height) * widthRatio;
		if (outputCross > fixedCross) {
			return {
				.width = static_cast<std::uint32_t>(
					static_cast<std::uint64_t>(a_output.height) * widthRatio / heightRatio),
				.height = a_output.height,
			};
		}
		if (outputCross < fixedCross) {
			return {
				.width = a_output.width,
				.height = static_cast<std::uint32_t>(
					static_cast<std::uint64_t>(a_output.width) * heightRatio / widthRatio),
			};
		}
		return a_output;
	}
}
