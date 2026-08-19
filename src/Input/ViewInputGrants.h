#pragma once

#include <string>
#include <string_view>
#include <unordered_map>
#include <unordered_set>

#include "Input/GamepadSession.h"

namespace OSFUI
{
	class ViewInputGrants
	{
	public:
		void SetGamepadMode(std::string_view a_viewId, GamepadSession::Mode a_mode);
		void SetBackOwnership(std::string_view a_viewId, bool a_enabled);

		GamepadSession::Mode GamepadModeFor(std::string_view a_viewId) const;
		bool                 OwnsBackAction(std::string_view a_viewId) const;

        void ResetPage(std::string_view a_viewId);
        void ResetAll();

    private:
		std::unordered_map<std::string, GamepadSession::Mode> m_gamepadModes;
		std::unordered_set<std::string>                       m_backOwners;
	};
}
