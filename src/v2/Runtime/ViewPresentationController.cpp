#include "ViewPresentationController.h"

namespace Runtime
{
    bool ViewPresentationController::Open(const ViewManifest& a_view)
    {
        if (a_view.kind == ViewKind::Hud) {
            return _shownHuds.insert(a_view.id).second;
        }

        const bool changed = !_activeMenu || *_activeMenu != a_view.id || _menuCapturesInput != a_view.capturesInput || _menuPausesGame != a_view.pausesGame;

        _activeMenu = a_view.id;
        _menuCapturesInput = a_view.capturesInput;
        _menuPausesGame = a_view.pausesGame;

        return changed;
    }

    bool ViewPresentationController::Close(std::string_view a_id)
    {
        if (_shownHuds.erase(std::string{ a_id }) != 0) {
            return true;
        }

        if (_activeMenu && *_activeMenu == a_id) {
            _activeMenu.reset();
            _menuCapturesInput = false;
            _menuPausesGame = false;
            return true;
        }

        return false;
    }

    bool ViewPresentationController::CloseActiveMenu()
    {
        if (!_activeMenu) {
            return false;
        }

        _activeMenu.reset();
        _menuCapturesInput = false;
        _menuPausesGame = false;
        return true;
    }

    void ViewPresentationController::CloseAll()
    {
        _activeMenu.reset();
        _shownHuds.clear();

        _menuCapturesInput = false;
        _menuPausesGame = false;
    }

    bool ViewPresentationController::IsOpen(std::string_view a_id) const
    {
        if (_activeMenu && *_activeMenu == a_id) {
            return true;
        }

        return _shownHuds.contains(std::string{ a_id });
    }

    std::optional<std::string> ViewPresentationController::ActiveMenu() const
    {
        return _activeMenu;
    }

    bool ViewPresentationController::CapturesInput() const
    {
        return _activeMenu && _menuCapturesInput;
    }

    bool ViewPresentationController::PausesGame() const
    {
        return _activeMenu && _menuPausesGame;
    }

    std::vector<std::string> ViewPresentationController::OpenViewIds() const
    {
        std::vector<std::string> ids;
        ids.reserve(_shownHuds.size() + (_activeMenu ? 1 : 0));

        for (const auto& id : _shownHuds) {
            ids.push_back(id);
        }

        if (_activeMenu) {
            ids.push_back(*_activeMenu);
        }

        std::ranges::sort(ids);
        return ids;
    }
}