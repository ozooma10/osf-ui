#pragma once

namespace OSFUI
{
	// Runtime-owned: balance the MenuCursor free-cursor reference or gameplay mouse-look stays unpinned.
	class FreeCursor
	{
	public:
		// Drive the reference from each runtime tick, retrying until MenuCursor exists.
		static void Apply(bool a_desired);
	};
}
