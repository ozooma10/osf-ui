#pragma once

#include <cstdint>

namespace OSFUI
{
	// Starfields input delivery stops when webview2 has focus, so poll XInput directly for gamepad events.
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

		[[nodiscard]] State Poll(double a_now);
		void Reset();

	private:
		std::uint32_t m_latchedSlot{ 4 };
		double        m_nextDiscoveryAt{ 0.0 };  // empty-slot probes are throttled while nothing is connected
	};
}
