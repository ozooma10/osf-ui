#include "Views/ViewRevealGate.h"

#include <algorithm>

namespace OSFUI
{
	void ViewRevealGate::Arm()
	{
		m_pending = true;
		m_frameReady = false;
		m_heldSeconds = 0.0;
		m_lastPolledAt.reset();
	}

	void ViewRevealGate::Cancel()
	{
		m_pending = false;
		m_frameReady = false;
		m_heldSeconds = 0.0;
		m_lastPolledAt.reset();
	}

	void ViewRevealGate::Reset()
	{
		Cancel();
		m_lastSubmittedFrame = 0;
	}

	ViewRevealGate::Decision ViewRevealGate::Observe(const std::optional<FrameObservation>& a_frame, double a_nowSeconds)
	{
		Decision decision;
		if (a_frame) {
			if (!m_pending) {
				m_lastSubmittedFrame = a_frame->index;
				decision.submitFrame = true;
				return decision;
			}

			if (a_frame->index != m_lastSubmittedFrame) {
				m_lastSubmittedFrame = a_frame->index;
				m_frameReady = true;
				decision.submitFrame = true;
			}

			if (m_frameReady && a_frame->outputSizeKnown && a_frame->matchesExpectedSize) {
				Cancel();
				decision.reveal = true;
				return decision;
			}
		} else if (!m_pending) {
			return decision;
		}

		// frame wins before checking timeout. Prevents alt-tab or load hitch from timing out frame that became ready on first resumed tick.
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
