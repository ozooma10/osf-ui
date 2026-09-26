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
		bool SetSuspended(bool a_suspended);          // retain HUD intent, close menus
		void CloseAll();                              // close the menu and every shown HUD

		// Runtime consumes changes at a presentation boundary. Idle ticks still
		// reconcile engine input, but do not rebuild layers or resend view state.
		bool TakeChanged();
		void Invalidate() { m_changed = true; } // replay policy after host replacement

		// Derived desired state — read in the runtime update after any change.
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

		std::unordered_map<std::string, InstantiatedView> m_instantiated;
		std::optional<std::string>                        m_activeMenu;
		std::unordered_set<std::string>                   m_hudShown;
		bool m_suspended{ false };
		bool m_changed{ false };
	};
}
