#include "ViewInputGrants.h"

namespace OSFUI
{
    void ViewInputGrants::SetGamepadRaw(std::string_view a_viewId, bool a_enabled)
    {
        if (a_enabled) {
            m_gamepadRaw.emplace(a_viewId);
        } else {
            m_gamepadRaw.erase(std::string(a_viewId));
        }
    }

    void ViewInputGrants::SetBackOwnership(std::string_view a_viewId, bool a_enabled)
    {
        if (a_enabled) {
            m_backOwners.emplace(a_viewId);
        } else {
            m_backOwners.erase(std::string(a_viewId));
        }
    }

    bool ViewInputGrants::UsesRawGamepad(std::string_view a_viewId) const
    {
        return m_gamepadRaw.contains(std::string(a_viewId));
    }

    bool ViewInputGrants::OwnsBackAction(std::string_view a_viewId) const
    {
        return m_backOwners.contains(std::string(a_viewId));
    }

    void ViewInputGrants::ResetPage(std::string_view a_viewId)
    {
        const std::string id(a_viewId);
		m_gamepadRaw.erase(id);
		m_backOwners.erase(id);
    }

    void ViewInputGrants::ResetAll()
    {
        m_gamepadRaw.clear();
        m_backOwners.clear();
    }
}