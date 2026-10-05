#pragma once

namespace OSFUI
{
	// Runtime owns one free-cursor reference; release it to restore mouse-look.
	class FreeCursor
	{
	public:
		// Call every runtime tick; retries until MenuCursor exists.
		static void Apply(bool a_desired);
	};
}
