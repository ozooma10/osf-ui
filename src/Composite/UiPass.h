#pragma once

namespace OSFUI::UiPass
{
	// Hook starfield scaleform ui render pass to draw under/over it. recording browser quad at ScaleformEnd handoff puts it upstream of frame composition and frame gen.
	// With FG active, first RT->pixel-SRV candidate is opaque interpolation input, not UI layer. handoff shape identifies that graph and skips opaque candidate and writes only transparent COPY_SOURCE handoff consumed by FFX.
	bool Install();

	bool DrawEnabled();
}
