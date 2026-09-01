#pragma once

#include <cstdint>

namespace OSFUI::UiPass
{
	// Vanilla and unknown composite owners draw at ScaleformEnd; proven foreign
	// owners may opt into a validated post-ScaleformComposite target.
	bool Install();
	void SetExpectedOutputSize(std::uint32_t a_width, std::uint32_t a_height);

	bool DrawEnabled();
	bool UsesScaleformEnd();
	bool FrameGenerationActive();
}
