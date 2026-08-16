#pragma once

#include <optional>
#include <string>
#include <string_view>

namespace OSFUI
{
    class ViewOpenSession
    {
    public:
        void Begin(std::string_view a_target, double a_now, bool a_alreadyLoaded);
        void Cancel();

        bool Active() const;
        bool Targets(std::string_view a_viewId) const;
        std::string_view Target() const;

        bool UpdateHandoff(std::string_view a_phase, bool a_error);

        bool RetryRequested() const;
        bool RequestRetry();
        bool TakeRetryRequest();

        void Restart(double a_now);
        void NoteLoaded(double a_now);

        bool ReadySignalTimedOut(double a_now, double a_timeout) const;
        bool HandoffDelayElapsed(double a_now, double a_delay) const;

    private:
        struct State
        {
            std::string target;
            double startedAt{ 0.0 };
            double loadedAt{ -1.0 };
            std::string phase;
            bool handoffVisible{ false };
            bool error { false };
            bool retryRequested{ false };
        };

        std::optional<State> m_state;
    };
}