#include "runtime/MenuController.h"

#include <algorithm>

namespace OSFUI
{
	void MenuController::Register(const Surface& a_surface)
	{
		_registry[a_surface.id] = a_surface;
	}

	const MenuController::Surface* MenuController::Find(std::string_view a_id) const
	{
		const auto it = _registry.find(std::string(a_id));
		return it == _registry.end() ? nullptr : &it->second;
	}

	bool MenuController::IsOpen(std::string_view a_id) const
	{
		const std::string id(a_id);
		return _hudShown.contains(id) || std::ranges::find(_menuStack, id) != _menuStack.end();
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

		// Menu - single-menu policy: only one menu open at a time. If it is already the sole open menu, nothing changes; otherwise it replaces the stack.
		if (_menuStack.size() == 1 && _menuStack.back() == id) {
			return false;
		}
		_menuStack.clear();
		_menuStack.push_back(id);
		return true;
	}

	bool MenuController::Close(std::string_view a_id)
	{
		const std::string id(a_id);
		if (_hudShown.erase(id) > 0) {
			return true;
		}
		if (const auto it = std::ranges::find(_menuStack, id); it != _menuStack.end()) {
			_menuStack.erase(it);
			return true;
		}
		return false;
	}

	bool MenuController::CloseTop()
	{
		if (_menuStack.empty()) {
			return false;
		}
		_menuStack.pop_back();
		return true;
	}

	void MenuController::CloseAll()
	{
		// Clears BOTH so DesiredVisible() goes false, a lingering shown HUD would otherwise keep the overlay up across a save/load or main-menu transition.
		_menuStack.clear();
		_hudShown.clear();
	}

	bool MenuController::ToggleDefault(std::string_view a_defId)
	{
		return _menuStack.empty() ? Open(a_defId) : CloseTop();
	}

	bool MenuController::DesiredVisible() const
	{
		return !_hudShown.empty() || !_menuStack.empty();
	}

	bool MenuController::DesiredCapture() const
	{
		if (_menuStack.empty()) {
			return false;
		}
		const auto* surface = Find(_menuStack.back());
		return surface && surface->capturesInput;
	}

	bool MenuController::DesiredPause() const
	{
		if (_menuStack.empty()) {
			return false;
		}
		const auto* surface = Find(_menuStack.back());
		return surface && surface->pausesGame;
	}

	std::optional<std::string> MenuController::ActiveMenu() const
	{
		if (_menuStack.empty()) {
			return std::nullopt;
		}
		return _menuStack.back();
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
			} else if (const auto it = std::ranges::find(_menuStack, id); it != _menuStack.end()) {
				layer.hidden = false;
				layer.z = 1000 + static_cast<int>(std::distance(_menuStack.begin(), it));
			} else {
				layer.hidden = true;
				layer.z = 1000;  // menu band; hidden, so exact value is immaterial
			}
			layers.push_back(std::move(layer));
		}
		return layers;
	}
}
