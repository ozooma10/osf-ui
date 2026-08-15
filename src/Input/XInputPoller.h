#pragma once

#include <cstdint>

namespace OSFUI
{
	// Focus-independent controller state for an input-capturing WebView session.
	// Starfield's Windows.Gaming.Input delivery stops while another process owns
	// foreground focus, so Runtime polls XInput only for that interval.
	class XInputPoller
	{
	public:
		struct State
		{
			bool          connected{ false };
			std::uint32_t buttons{ 0 };
			float         lx{ 0.0f };
			float         ly{ 0.0f };
			float         rx{ 0.0f };
			float         ry{ 0.0f };
		};

		[[nodiscard]] static State Poll();

		// Forget which slot the current capturing interval latched onto (the
		// slot showing real input wins and then keeps winning). Called when
		// direct polling ends so the next overlay session re-picks the pad the
		// player is actually holding.
		static void ResetSlotLatch();
	};
}
