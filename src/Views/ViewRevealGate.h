#pragma once

#include <cstdint>
#include <optional>

namespace OSFUI
{
	// Policy for revealing a presentation. Decides whether a frame is fresh enough to submit, reveal or wait expired.
	class ViewRevealGate
	{
	public:
		static constexpr double kTimeoutSeconds = 3.0;
		static constexpr double kMaxHeldStepSeconds = 0.25;

		struct FrameObservation
		{
			std::uint64_t index{ 0 };
			bool          outputSizeKnown{ false };
			bool          matchesExpectedSize{ false };
		};

		struct Decision
		{
			bool   submitFrame{ false };
			bool   reveal{ false };
			bool   timedOut{ false };
			double heldSeconds{ 0.0 };
		};

		// Arm on closed-to-open edge. Most recent frame remains baseline, so cached content cannot satisfy this presentation.
		void Arm();
		void Cancel();
		void Reset();

		Decision Observe(const std::optional<FrameObservation>& a_frame, double a_nowSeconds);

		bool Pending() const { return m_pending; }

	private:
		bool                  m_pending{ false };
		bool                  m_frameReady{ false };
		double                m_heldSeconds{ 0.0 };
		std::optional<double> m_lastPolledAt;
		std::uint64_t         m_lastSubmittedFrame{ 0 };
	};
}
