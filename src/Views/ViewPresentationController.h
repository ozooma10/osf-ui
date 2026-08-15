#pragma once

#include <optional>
#include <unordered_set>  // not in pch.h

#include "Views/ViewManifest.h"  // ViewKind

namespace OSFUI
{
	// Instantiated views (menus + HUDs) and their open state. One menu may occupy
	// the active-menu slot while any number of HUDs remain shown.
	class ViewPresentationController
	{
	public:
		struct InstantiatedView
		{
			std::string id;
			ViewKind    kind{ ViewKind::Menu };
			bool        capturesInput{ true };
			bool        pausesGame{ false };
			int         order{ 0 };  // within-band z hint
		};

		// Add (or replace) an instantiated view by qualified id. Idempotent.
		void AddInstantiated(const InstantiatedView& a_view);

		// Remove a view entirely (closing it first if open). Returns true if the
		// open-state changed (caller must re-apply policy). Called when a view is torn
		// down at runtime (crash-recovery exhaustion) so nothing can reopen a view
		// whose renderer view no longer exists.
		bool RemoveInstantiated(std::string_view a_id);

		// State transitions; each returns true if the open-state changed. Unknown ids return false.
		// Open: a menu replaces the current menu; a HUD is added to the shown set.
		bool Open(std::string_view a_id);
		bool Close(std::string_view a_id);
		bool CloseActiveMenu();                       // HUDs untouched
		void CloseAll();                              // close the menu and every shown HUD

		// Derived desired state — read on the main thread after any change.
		[[nodiscard]] bool DesiredVisible() const;  // any HUD shown || any menu open
		[[nodiscard]] bool DesiredCapture() const;  // active menu && capturesInput
		[[nodiscard]] bool DesiredPause() const;    // active menu && pausesGame
		[[nodiscard]] std::optional<std::string> ActiveMenu() const;
		[[nodiscard]] bool IsOpen(std::string_view a_id) const;
		[[nodiscard]] bool IsInstantiated(std::string_view a_id) const;

		// One entry per instantiated view with its hidden flag and composite z.
		// HUD band [0..999] = clamp(order); the active menu sits at 1000 above every HUD.
		struct Layer
		{
			std::string id;
			bool        hidden{ true };
			int         z{ 0 };
		};
		[[nodiscard]] std::vector<Layer> DesiredLayers() const;

	private:
		[[nodiscard]] const InstantiatedView* FindInstantiated(std::string_view a_id) const;

		std::unordered_map<std::string, InstantiatedView> _instantiated;
		std::optional<std::string>                        _activeMenu;
		std::unordered_set<std::string>                   _hudShown;
	};
}
