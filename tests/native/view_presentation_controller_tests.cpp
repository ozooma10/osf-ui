// ViewPresentationController: instantiated views, one active-menu slot, HUD shown
// set, and the derived policy/layers Runtime applies after every change.
// Web-renderer-independent state machine; previously the largest untested runtime
// module. The tests assert the active-menu invariant so a future multi-menu
// change trips a test, not a policy bug.

#include "Views/ViewPresentationController.h"

#include <cassert>
#include <iostream>

using OSFUI::ViewKind;
using OSFUI::ViewPresentationController;

namespace
{
	ViewPresentationController::InstantiatedView Menu(std::string a_id, bool a_captures = true, bool a_pauses = false)
	{
		return { std::move(a_id), ViewKind::Menu, a_captures, a_pauses, 0 };
	}

	ViewPresentationController::InstantiatedView Hud(std::string a_id, int a_order = 0)
	{
		return { std::move(a_id), ViewKind::Hud, false, false, a_order };
	}

	const ViewPresentationController::Layer* LayerOf(const std::vector<ViewPresentationController::Layer>& a_layers,
		std::string_view a_id)
	{
		for (const auto& layer : a_layers) {
			if (layer.id == a_id) return &layer;
		}
		return nullptr;
	}
}

int main()
{
	// Unknown ids: every transition refuses, nothing changes.
	{
		ViewPresentationController controller;
		assert(!controller.Open("nope"));
		assert(!controller.Close("nope"));
		assert(!controller.CloseActiveMenu());
		assert(!controller.IsOpen("nope"));
		assert(!controller.IsInstantiated("nope"));
		assert(!controller.DesiredVisible());
		assert(!controller.ActiveMenu());
	}

	// Single-menu policy: a second menu REPLACES the first, and reopening the
	// sole open menu reports no change.
	{
		ViewPresentationController controller;
		controller.AddInstantiated(Menu("a/one"));
		controller.AddInstantiated(Menu("a/two"));
		assert(controller.Open("a/one"));
		assert(!controller.Open("a/one"));  // already the sole open menu
		assert(controller.ActiveMenu() == std::optional<std::string>("a/one"));
		assert(controller.Open("a/two"));
		assert(controller.ActiveMenu() == std::optional<std::string>("a/two"));
		assert(!controller.IsOpen("a/one"));  // replaced, not stacked
		// The active-menu invariant guarantees there is no hidden menu stack whose
		// stale depth could affect replacement.
		assert(controller.DesiredLayers().size() == 2);
		int visibleMenus = 0;
		for (const auto& layer : controller.DesiredLayers()) {
			if (!layer.hidden) visibleMenus++;
		}
		assert(visibleMenus == 1);
	}

	// Derived policy follows the active menu's flags.
	{
		ViewPresentationController controller;
		controller.AddInstantiated(Menu("a/pausing", /*captures=*/true, /*pauses=*/true));
		controller.AddInstantiated(Menu("a/passive", /*captures=*/false, /*pauses=*/false));
		assert(controller.Open("a/pausing"));
		assert(controller.DesiredCapture() && controller.DesiredPause() && controller.DesiredVisible());
		assert(controller.Open("a/passive"));
		assert(!controller.DesiredCapture() && !controller.DesiredPause() && controller.DesiredVisible());
		assert(controller.CloseActiveMenu());
		assert(!controller.DesiredVisible());
		assert(!controller.CloseActiveMenu());  // no active menu refuses
	}

	// HUDs: a shown set independent of the active-menu slot; the menu sits above every
	// HUD in the composite z bands.
	{
		ViewPresentationController controller;
		controller.AddInstantiated(Hud("a/hud", 5));
		controller.AddInstantiated(Hud("a/hud2", 2000));  // order clamps into the HUD band
		controller.AddInstantiated(Menu("a/menu"));
		assert(controller.Open("a/hud"));
		assert(!controller.Open("a/hud"));  // already shown
		assert(controller.DesiredVisible());
		assert(!controller.DesiredCapture());  // HUDs never capture
		assert(!controller.ActiveMenu());
		assert(controller.Open("a/hud2"));
		assert(controller.Open("a/menu"));
		const auto layers = controller.DesiredLayers();
		const auto* hud = LayerOf(layers, "a/hud");
		const auto* hud2 = LayerOf(layers, "a/hud2");
		const auto* menu = LayerOf(layers, "a/menu");
		assert(hud && !hud->hidden && hud->z == 5);
		assert(hud2 && !hud2->hidden && hud2->z == 999);  // clamped
		assert(menu && !menu->hidden && menu->z >= 1000);
		// Closing the active menu leaves the HUDs shown.
		assert(controller.CloseActiveMenu());
		assert(controller.IsOpen("a/hud") && controller.IsOpen("a/hud2"));
		assert(controller.DesiredVisible());
	}

	// Close() picks the right collection; CloseAll clears both.
	{
		ViewPresentationController controller;
		controller.AddInstantiated(Hud("a/hud"));
		controller.AddInstantiated(Menu("a/menu"));
		assert(controller.Open("a/hud"));
		assert(controller.Open("a/menu"));
		assert(controller.Close("a/menu"));
		assert(!controller.Close("a/menu"));  // already closed
		assert(controller.Close("a/hud"));
		assert(controller.Open("a/hud"));
		assert(controller.Open("a/menu"));
		controller.CloseAll();
		assert(!controller.DesiredVisible());
		assert(!controller.IsOpen("a/hud") && !controller.IsOpen("a/menu"));
		assert(controller.IsInstantiated("a/hud") && controller.IsInstantiated("a/menu"));
	}

	// Removing an instantiated view closes it first (true = policy must be re-applied) and makes the
	// id unopenable — the crash-recovery teardown path.
	{
		ViewPresentationController controller;
		controller.AddInstantiated(Menu("a/menu"));
		assert(controller.Open("a/menu"));
		assert(controller.RemoveInstantiated("a/menu"));   // was open: state changed
		assert(!controller.IsInstantiated("a/menu"));
		assert(!controller.Open("a/menu"));
		controller.AddInstantiated(Menu("a/menu"));
		assert(!controller.RemoveInstantiated("a/menu"));  // closed: no state change
	}

	// Register is idempotent and replaces flags in place.
	{
		ViewPresentationController controller;
		controller.AddInstantiated(Menu("a/menu", true, false));
		assert(controller.Open("a/menu"));
		controller.AddInstantiated(Menu("a/menu", true, true));  // dev-reload flag change
		assert(controller.DesiredPause());
		assert(controller.IsOpen("a/menu"));  // replacement does not close it
	}

	std::cout << "view presentation controller tests passed\n";
}
