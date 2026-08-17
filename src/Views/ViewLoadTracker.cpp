#include "Views/ViewLoadTracker.h"

namespace OSFUI
{
    void ViewLoadTracker::BeginLoad(std::string_view a_id)
    {
        m_states[std::string(a_id)] = ViewLoadState::Loading;
    }

    void ViewLoadTracker::FinishLoad(std::string_view a_id, bool a_failed)
    {
        m_states[std::string(a_id)] = a_failed ? ViewLoadState::Failed : ViewLoadState::Finished;
    }

    void ViewLoadTracker::Forget(std::string_view a_id)
    {
        m_states.erase(std::string(a_id));
    }

    ViewLoadState ViewLoadTracker::GetState(std::string_view a_id) const
    {
        const auto it = m_states.find(std::string(a_id));
        return it != m_states.end() ? it->second : ViewLoadState::Loading;
    }
}
