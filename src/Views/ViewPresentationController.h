#pragma once

#include <optional>
#include <unordered_set>  // not in pch.h

#include "Views/ViewManifest.h"  // ViewKind

namespace OSFUI
{
	// Track one active menu and any number of shown HUDs among instantiated views.
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

		// Remove the view and report whether closing it requires policy reapplication.
		bool RemoveInstantiated(std::string_view a_id);

		// State transitions report changes; opening a menu replaces the active one while HUDs accumulate.
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

		// Return each view's visibility and z, with HUDs at 0..999 and the active menu at 1000.
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
