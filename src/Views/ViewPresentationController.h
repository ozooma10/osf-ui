#pragma once

#include <optional>
#include <unordered_set>  // not in pch.h

#include "Views/ViewManifest.h"  // ViewKind

namespace OSFUI
{
	// One active menu; any number of visible HUDs.
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

		// Adds or replaces by qualified id.
		void AddInstantiated(const InstantiatedView& a_view);

		// Returns true if policy must be reapplied.
		bool RemoveInstantiated(std::string_view a_id);

		// Transitions return whether state changed. A menu replaces the active one; HUDs accumulate.
		// While suspended, menus are refused and HUD intent is kept.
		bool Open(std::string_view a_id);
		bool Close(std::string_view a_id);
		bool CloseActiveMenu();                       // HUDs untouched
		bool SetSuspended(bool a_suspended);          // retain HUD intent, close menus
		bool Suspended() const { return m_suspended; }

		// Clear after applying changes; unchanged ticks leave layers and view state alone.
		bool TakeChanged();
		void Invalidate() { m_changed = true; } // replay policy after host replacement

		[[nodiscard]] bool DesiredVisible() const;  // any HUD shown || any menu open
		[[nodiscard]] bool DesiredCapture() const;  // active menu && capturesInput
		[[nodiscard]] bool DesiredPause() const;    // active menu && pausesGame
		[[nodiscard]] std::optional<std::string> ActiveMenu() const;
		[[nodiscard]] bool IsOpen(std::string_view a_id) const;
		[[nodiscard]] bool IsInstantiated(std::string_view a_id) const;

		// HUDs at z 0..999, the active menu at 1000.
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
