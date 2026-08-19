#include "ViewInputGrants.h"

namespace OSFUI
{
	void ViewInputGrants::SetGamepadMode(std::string_view a_viewId, GamepadSession::Mode a_mode)
	{
		if (a_mode == GamepadSession::Mode::kDefault) {
			m_gamepadModes.erase(std::string(a_viewId));
		} else {
			m_gamepadModes.insert_or_assign(std::string(a_viewId), a_mode);
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

	GamepadSession::Mode ViewInputGrants::GamepadModeFor(std::string_view a_viewId) const
	{
		const auto it = m_gamepadModes.find(std::string(a_viewId));
		return it == m_gamepadModes.end() ? GamepadSession::Mode::kDefault : it->second;
    }

    bool ViewInputGrants::OwnsBackAction(std::string_view a_viewId) const
    {
        return m_backOwners.contains(std::string(a_viewId));
    }

	void ViewInputGrants::ResetPage(std::string_view a_viewId)
	{
		const std::string id(a_viewId);
		m_gamepadModes.erase(id);
		m_backOwners.erase(id);
    }

    void ViewInputGrants::ResetAll()
    {
		m_gamepadModes.clear();
        m_backOwners.clear();
    }
}
