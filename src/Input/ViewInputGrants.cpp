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

    void ViewInputGrants::SetBackOwnership(std::string_view a_viewId, bool a_enabled, std::string_view a_targetView)
    {
        if (a_enabled) {
            m_backActions.insert_or_assign(std::string(a_viewId), std::string(a_targetView));
        } else {
            m_backActions.erase(std::string(a_viewId));
        }
    }

	GamepadSession::Mode ViewInputGrants::GamepadModeFor(std::string_view a_viewId) const
	{
		const auto it = m_gamepadModes.find(std::string(a_viewId));
		return it == m_gamepadModes.end() ? GamepadSession::Mode::kDefault : it->second;
    }

    bool ViewInputGrants::OwnsBackAction(std::string_view a_viewId) const
    {
        return m_backActions.contains(std::string(a_viewId));
    }

    std::optional<std::string> ViewInputGrants::BackTargetFor(std::string_view a_viewId) const
    {
        const auto it = m_backActions.find(std::string(a_viewId));
        if (it == m_backActions.end() || it->second.empty()) {
            return std::nullopt;
        }
        return it->second;
    }

	void ViewInputGrants::ResetPage(std::string_view a_viewId)
	{
		const std::string id(a_viewId);
		m_gamepadModes.erase(id);
		m_backActions.erase(id);
    }

    void ViewInputGrants::ResetAll()
    {
		m_gamepadModes.clear();
        m_backActions.clear();
    }
}
