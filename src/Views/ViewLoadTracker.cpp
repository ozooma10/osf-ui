#include "Views/ViewLoadTracker.h"

namespace OSFUI
{
    void ViewLoadTracker::BeginLoad(std::string_view a_id)
    {
        const std::string id(a_id);
        m_states[id] = ViewLoadState::Loading;
        m_contentReady.erase(id);
    }

    void ViewLoadTracker::FinishLoad(std::string_view a_id, bool a_failed)
    {
        m_states[std::string(a_id)] = a_failed ? ViewLoadState::Failed : ViewLoadState::Finished;
    }

    void ViewLoadTracker::MarkContentReady(std::string_view a_id)
    {
        m_contentReady.emplace(a_id);
    }

    void ViewLoadTracker::ClearContentReady(std::string_view a_id)
    {
        m_contentReady.erase(std::string(a_id));
    }

    void ViewLoadTracker::ClearAllContentReady()
    {
        m_contentReady.clear();
    }

    void ViewLoadTracker::Forget(std::string_view a_id)
    {
        const std::string id(a_id);
        m_states.erase(id);
        m_contentReady.erase(id);
    }

    ViewLoadState ViewLoadTracker::GetState(std::string_view a_id) const
    {
        const auto it = m_states.find(std::string(a_id));
        return it != m_states.end() ? it->second : ViewLoadState::Loading;
    }

    bool ViewLoadTracker::IsRevealReady(std::string_view a_id, bool a_requiresReadySignal) const
    {
        if(a_requiresReadySignal) {
            return m_contentReady.contains(std::string(a_id));
        }
        return GetState(a_id) == ViewLoadState::Finished;
    }
}