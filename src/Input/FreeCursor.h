#pragma once

namespace OSFUI
{
	// Main-thread only: balance the MenuCursor free-cursor reference or gameplay mouse-look stays unpinned.
	class FreeCursor
	{
	public:
		// Drive the reference from each main-thread tick, retrying until MenuCursor exists.
		static void Apply(bool a_desired);
	};
}
