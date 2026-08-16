#pragma once

#include <string>
#include <string_view>
#include <unordered_set>

namespace OSFUI
{
    class ViewInputGrants
    {
    public:
        void SetGamepadRaw(std::string_view a_viewId, bool a_enabled);
        void SetBackOwnership(std::string_view a_viewId, bool a_enabled);

        bool UsesRawGamepad(std::string_view a_viewId) const;
        bool OwnsBackAction(std::string_view a_viewId) const;

        void ResetPage(std::string_view a_viewId);
        void ResetAll();

    private:
        std::unordered_set<std::string> m_gamepadRaw;
        std::unordered_set<std::string> m_backOwners;
    };
}