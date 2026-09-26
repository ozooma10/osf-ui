#pragma once

#include <optional>

namespace OSFUI
{
	// Holds a presentation hidden until a frame at the expected size arrives, or times out.
	// Every arm site has already invalidated the cached frame (new epoch, compositor hide, or new ring), so any frame is fresh.
	class ViewRevealGate
	{
	public:
		static constexpr double kTimeoutSeconds = 3.0;
		static constexpr double kMaxHeldStepSeconds = 0.25;

		struct Decision
		{
			bool   reveal{ false };
			bool   timedOut{ false };
			double heldSeconds{ 0.0 };
		};

		void Arm();
		void Cancel();

		Decision Observe(bool a_frameReady, double a_nowSeconds);

		bool Pending() const { return m_pending; }

	private:
		bool                  m_pending{ false };
		double                m_heldSeconds{ 0.0 };
		std::optional<double> m_lastPolledAt;
	};
}
