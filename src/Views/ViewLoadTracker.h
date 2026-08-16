#pragma once

#include <string>
#include <string_view>
#include <unordered_set>
#include <unordered_map>

namespace OSFUI
{
    enum class ViewLoadState
    {
        Loading,
        Finished,
        Failed
    };

    class ViewLoadTracker
    {
    public:
        void BeginLoad(std::string_view a_id);
        void FinishLoad(std::string_view a_id, bool a_failed);

        void MarkContentReady(std::string_view a_id);
        void ClearContentReady(std::string_view a_id);
        void ClearAllContentReady();

        void Forget(std::string_view a_id);

        ViewLoadState GetState(std::string_view a_id) const;
        bool IsRevealReady(std::string_view a_id, bool a_requiresReadySignal) const;

    private:
        std::unordered_map<std::string, ViewLoadState> m_states;
        std::unordered_set<std::string> m_contentReady;
    };
}