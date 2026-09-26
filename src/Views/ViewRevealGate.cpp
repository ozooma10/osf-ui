#include "Views/ViewRevealGate.h"

#include <algorithm>

namespace OSFUI
{
	void ViewRevealGate::Arm()
	{
		m_pending = true;
		m_heldSeconds = 0.0;
		m_lastPolledAt.reset();
	}

	void ViewRevealGate::Cancel()
	{
		m_pending = false;
		m_heldSeconds = 0.0;
		m_lastPolledAt.reset();
	}

	ViewRevealGate::Decision ViewRevealGate::Observe(bool a_frameReady, double a_nowSeconds)
	{
		Decision decision;
		if (!m_pending) {
			return decision;
		}
		// frame wins before checking timeout. Prevents alt-tab or load hitch from timing out frame that became ready on first resumed tick.
		if (a_frameReady) {
			Cancel();
			decision.reveal = true;
			return decision;
		}

		if (m_lastPolledAt) {
			const auto elapsed = std::max(0.0, a_nowSeconds - *m_lastPolledAt);
			m_heldSeconds += std::min(elapsed, kMaxHeldStepSeconds);
		}
		m_lastPolledAt = a_nowSeconds;
		if (m_heldSeconds >= kTimeoutSeconds) {
			decision.timedOut = true;
			decision.heldSeconds = m_heldSeconds;
			Cancel();
		}
		return decision;
	}
}
