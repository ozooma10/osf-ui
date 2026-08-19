#pragma once

#include <array>
#include <cstdint>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

namespace OSFUI
{
    class ViewRecoveryTracker
    {
    public:
        static constexpr std::uint32_t kMaxAttempts = 3;

        struct FailureDecision
        {
            bool exhausted{ false };
            std::uint32_t attemptsCompleted{ 0 };
            std::uint32_t attemptsRemaining{ 0 };
            std::uint32_t nextAttempt{ 0 };
            double retryDelay{ 0.0 };
        };

        bool Clear(std::string_view a_viewId);
        void ClearAll();
        bool Contains(std::string_view a_viewId) const;

        FailureDecision ScheduleFailure(std::string_view a_viewId, double a_now);

        std::vector<std::string> TakeDue(double a_now);
        std::uint32_t BeginAttempt(std::string_view a_viewId);

    private:
        struct State
        {
            std::uint32_t attempts{ 0 };
            double retryAt{ 0.0 };
            bool pending{ false };
        };

        static constexpr std::array<double, kMaxAttempts> kBackoffSeconds{ 2.0, 5.0, 15.0 };

        std::unordered_map<std::string, State> m_states;
    };
}