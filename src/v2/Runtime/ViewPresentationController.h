#pragma once

#include "ViewManifest.h"

#include <unordered_set>

namespace Runtime
{
    class ViewPresentationController
    {
    public:
        bool Open(const ViewManifest& a_view);
        bool Close(std::string_view a_id);
        bool CloseActiveMenu();
        void CloseAll();

        bool IsOpen(std::string_view a_id) const;
        std::optional<std::string> ActiveMenu() const;

        bool CapturesInput() const;
        bool PausesGame() const;

        std::vector<std::string> OpenViewIds() const;

    private:
        std::optional<std::string> _activeMenu;
        std::unordered_set<std::string> _shownHuds;

        bool _menuCapturesInput{ false };
        bool _menuPausesGame{ false };
    };
}