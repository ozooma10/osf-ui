#pragma once

namespace OSFUI::UiPass
{
	// Draw at the ScaleformEnd handoff, skipping the opaque interpolation target under frame generation.
	bool Install();

	bool DrawEnabled();
	bool FrameGenerationActive();
}
