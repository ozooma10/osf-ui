#include "Views/ViewRecoveryTracker.h"

namespace OSFUI
{
    bool ViewRecoveryTracker::Clear(std::string_view a_viewId)
    {
        return m_states.erase(std::string(a_viewId)) > 0;
    }

    void ViewRecoveryTracker::ClearAll()
    {
        m_states.clear();
    }

    bool ViewRecoveryTracker::Contains(std::string_view a_viewId) const
    {
        return m_states.contains(std::string(a_viewId));
    }

    ViewRecoveryTracker::FailureDecision ViewRecoveryTracker::ScheduleFailure(std::string_view a_viewId, double a_now)
    {
        auto& state = m_states[std::string(a_viewId)];

        FailureDecision decision;
        decision.attemptsCompleted = state.attempts;

        if(state.attempts >= kMaxAttempts) {
            decision.exhausted = true;
            state.pending = false;
            return decision;
        }

        decision.nextAttempt = state.attempts + 1;
        decision.attemptsRemaining = kMaxAttempts - state.attempts;
        decision.retryDelay = kBackoffSeconds[state.attempts];

        state.pending = true;
        state.retryAt = a_now + decision.retryDelay;

        return decision;
    }

    std::vector<std::string> ViewRecoveryTracker::TakeDue(double a_now)
    {
        std::vector<std::string> due;
        for(auto& [viewId, state] : m_states) {
            if(state.pending && state.retryAt <= a_now) {
                due.push_back(viewId);
                state.pending = false;
            }
        }
        return due;
    }

    std::uint32_t ViewRecoveryTracker::BeginAttempt(std::string_view a_viewId)
    {
        const auto it = m_states.find(std::string(a_viewId));
        if(it == m_states.end()) {
            return 0;
        }

        if(it->second.attempts < kMaxAttempts) {
            ++it->second.attempts;
        }

        return it->second.attempts;
    }
}