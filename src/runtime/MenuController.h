#pragma once

#include <unordered_set>  // not in pch.h

#include "runtime/ViewManifest.h"  // SurfaceKind

namespace OSFUI
{
	// Owns the set of registered surfaces (menus + HUDs), their open state, and derives the desired global UI policy from the top-of-stack surface.
	class MenuController
	{
	public:
		struct Surface
		{
			std::string id;
			SurfaceKind kind{ SurfaceKind::Menu };
			bool        capturesInput{ true };
			bool        pausesGame{ false };
			int         order{ 0 };  // within-band z hint
		};

		// Register (or replace) a surface by id. Idempotent.
		void Register(const Surface& a_surface);

		// Remove a surface entirely (closing it first if open). Returns true if
		// the open-state changed (caller should re-apply policy). Used when a
		// view is torn down at runtime (crash-recovery exhaustion) so nothing
		// can reopen a surface whose renderer view no longer exists.
		bool Unregister(std::string_view a_id);

		// State transitions. Each returns true if the open-state actually changed.
		// Open: a menu is pushed (single-menu policy: it replaces the current menu); a HUD is added to the shown set.
		// Unknown ids return false.
		bool Open(std::string_view a_id);
		bool Close(std::string_view a_id);
		bool CloseTop();                               // pop the top menu (HUDs untouched)
		void CloseAll();                              // clear the stack AND every shown HUD
		bool ToggleDefault(std::string_view a_defId);  // stack empty ? Open(def) : CloseTop()

		// Derived desired state — read on the main thread after any change.
		[[nodiscard]] bool DesiredVisible() const;  // any HUD shown || any menu open
		[[nodiscard]] bool DesiredCapture() const;  // top menu && capturesInput
		[[nodiscard]] bool DesiredPause() const;    // top menu && pausesGame (reserved; unused in Steps 1-2)
		[[nodiscard]] std::optional<std::string> ActiveMenu() const;  // stack top = focus target
		[[nodiscard]] bool IsOpen(std::string_view a_id) const;
		[[nodiscard]] bool IsRegistered(std::string_view a_id) const;

		// One entry per registered surface with its computed hidden flag and composite z. HUD band [0..999] = clamp(order); menu band = 1000 + stack index (so any open menu sits above every HUD).
		struct Layer
		{
			std::string id;
			bool        hidden{ true };
			int         z{ 0 };
		};
		[[nodiscard]] std::vector<Layer> DesiredLayers() const;

	private:
		[[nodiscard]] const Surface* Find(std::string_view a_id) const;

		std::unordered_map<std::string, Surface> _registry;
		std::vector<std::string>                 _menuStack;  // top = back()
		std::unordered_set<std::string>          _hudShown;
	};
}
