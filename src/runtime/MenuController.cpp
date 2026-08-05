#include "runtime/MenuController.h"

#include <algorithm>

namespace OSFUI
{
	void MenuController::Register(const Surface& a_surface)
	{
		_registry[a_surface.id] = a_surface;
	}

	bool MenuController::Unregister(std::string_view a_id)
	{
		const bool changed = Close(a_id);
		_registry.erase(std::string(a_id));
		return changed;
	}

	const MenuController::Surface* MenuController::Find(std::string_view a_id) const
	{
		const auto it = _registry.find(std::string(a_id));
		return it == _registry.end() ? nullptr : &it->second;
	}

	bool MenuController::IsOpen(std::string_view a_id) const
	{
		const std::string id(a_id);
		return _hudShown.contains(id) || (_activeMenu && *_activeMenu == id);
	}

	bool MenuController::IsRegistered(std::string_view a_id) const
	{
		return Find(a_id) != nullptr;
	}

	bool MenuController::Open(std::string_view a_id)
	{
		const auto* surface = Find(a_id);
		if (!surface) {
			return false;  // not registered
		}
		const std::string id(a_id);

		if (surface->kind == SurfaceKind::Hud) {
			return _hudShown.insert(id).second;  // false if already shown
		}

		if (_activeMenu && *_activeMenu == id) {
			return false;
		}
		_activeMenu = id;
		return true;
	}

	bool MenuController::Close(std::string_view a_id)
	{
		const std::string id(a_id);
		if (_hudShown.erase(id) > 0) {
			return true;
		}
		if (_activeMenu && *_activeMenu == id) {
			_activeMenu.reset();
			return true;
		}
		return false;
	}

	bool MenuController::CloseTop()
	{
		if (!_activeMenu) {
			return false;
		}
		_activeMenu.reset();
		return true;
	}

	void MenuController::CloseAll()
	{
		// Clears BOTH so DesiredVisible() goes false; a lingering shown HUD would
		// otherwise keep the overlay up across a save/load or main-menu transition.
		_activeMenu.reset();
		_hudShown.clear();
	}

	bool MenuController::DesiredVisible() const
	{
		return !_hudShown.empty() || _activeMenu.has_value();
	}

	bool MenuController::DesiredCapture() const
	{
		if (!_activeMenu) {
			return false;
		}
		const auto* surface = Find(*_activeMenu);
		return surface && surface->capturesInput;
	}

	bool MenuController::DesiredPause() const
	{
		if (!_activeMenu) {
			return false;
		}
		const auto* surface = Find(*_activeMenu);
		return surface && surface->pausesGame;
	}

	std::optional<std::string> MenuController::ActiveMenu() const
	{
		return _activeMenu;
	}

	std::vector<MenuController::Layer> MenuController::DesiredLayers() const
	{
		std::vector<Layer> layers;
		layers.reserve(_registry.size());
		for (const auto& [id, surface] : _registry) {
			Layer layer;
			layer.id = id;
			if (surface.kind == SurfaceKind::Hud) {
				layer.hidden = !_hudShown.contains(id);
				layer.z = std::clamp(surface.order, 0, 999);
			} else if (_activeMenu && *_activeMenu == id) {
				layer.hidden = false;
				layer.z = 1000;
			} else {
				layer.hidden = true;
				layer.z = 1000;  // menu band; hidden, so exact value is immaterial
			}
			layers.push_back(std::move(layer));
		}
		return layers;
	}
}
