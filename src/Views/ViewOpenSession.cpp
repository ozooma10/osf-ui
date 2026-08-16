#include "Views/ViewOpenSession.h"

namespace OSFUI
{
    void ViewOpenSession::Begin(std::string_view a_target, double a_now, bool a_alreadyLoaded)
    {
        m_state.emplace();
        m_state->target = a_target;
        m_state->startedAt = a_now;
        m_state->loadedAt = a_alreadyLoaded ? a_now : -1.0;
    }

    void ViewOpenSession::Cancel()
    {
        m_state.reset();
    }

    bool ViewOpenSession::Active() const
    {
        return m_state.has_value();
    }

    bool ViewOpenSession::Targets(std::string_view a_viewId) const
    {
        return m_state.has_value() && m_state->target == a_viewId;
    }

    std::string_view ViewOpenSession::Target() const
    {
        return m_state.has_value() ? m_state->target : std::string_view{};
    }

    bool ViewOpenSession::UpdateHandoff(std::string_view a_phase, bool a_error)
    {
        if (!m_state.has_value()) {
            return false;
        }

        const bool changed = !m_state->handoffVisible || m_state->phase != a_phase || m_state->error != a_error;
        if(!changed) {
            return false;
        }

        m_state->phase = a_phase;
        m_state->error = a_error;
        m_state->handoffVisible = true;
        return true;
    }

    bool ViewOpenSession::RetryRequested() const
    {
        return m_state.has_value() && m_state->retryRequested;
    }

    bool ViewOpenSession::RequestRetry()
    {
        if (!m_state || !m_state->error) {
            return false;
        }

        m_state->retryRequested = true;
        return true;
    }

    bool ViewOpenSession::TakeRetryRequest()
    {
        if (!m_state.has_value() || !m_state->retryRequested) {
            return false;
        }

        m_state->retryRequested = false;
        return true;
    }

    void ViewOpenSession::Restart(double a_now)
    {
        if (!m_state) {
            return;
        }

        m_state->startedAt = a_now;
        m_state->loadedAt = -1.0;
        m_state->phase.clear();
        m_state->error = false;
        m_state->retryRequested = false;
    }

    void ViewOpenSession::NoteLoaded(double a_now)
    {
        if(m_state && m_state->loadedAt < 0.0) {
            m_state->loadedAt = a_now;
        }
    }

    bool ViewOpenSession::ReadySignalTimedOut(double a_now, double a_timeout) const
    {
        return m_state && m_state->loadedAt >= 0.0 && (a_now - m_state->loadedAt) >= a_timeout;
    }

    bool ViewOpenSession::HandoffDelayElapsed(double a_now, double a_delay) const
    {
        return m_state && a_now - m_state->startedAt >= a_delay;
    }
}