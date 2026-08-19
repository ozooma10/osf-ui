#pragma once

#include <optional>
#include <string>
#include <string_view>
#include <unordered_map>

#include "Input/GamepadSession.h"

namespace OSFUI
{
	class ViewInputGrants
	{
	public:
		void SetGamepadMode(std::string_view a_viewId, GamepadSession::Mode a_mode);
		void SetBackOwnership(std::string_view a_viewId, bool a_enabled, std::string_view a_targetView = {});

		GamepadSession::Mode GamepadModeFor(std::string_view a_viewId) const;
		bool                 OwnsBackAction(std::string_view a_viewId) const;
		std::optional<std::string> BackTargetFor(std::string_view a_viewId) const;

        void ResetPage(std::string_view a_viewId);
        void ResetAll();

    private:
		std::unordered_map<std::string, GamepadSession::Mode> m_gamepadModes;
		// Empty target means the view wants the synthetic Escape event. A non-empty target lets Runtime perform the menu handoff in the input tick.
		std::unordered_map<std::string, std::string>          m_backActions;
	};
}
