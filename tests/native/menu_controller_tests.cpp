// MenuController: registered surfaces, one optional active menu, HUD shown
// set, and the derived policy/layers Runtime applies after every change.
// Host-independent state machine; previously the largest untested runtime
// module. The tests assert the single-menu invariant so a future multi-menu
// change trips a test, not a policy bug.

#include "runtime/MenuController.h"

#include <cassert>
#include <iostream>

using OSFUI::MenuController;
using OSFUI::SurfaceKind;

namespace
{
	MenuController::Surface Menu(std::string a_id, bool a_captures = true, bool a_pauses = false)
	{
		return { std::move(a_id), SurfaceKind::Menu, a_captures, a_pauses, 0 };
	}

	MenuController::Surface Hud(std::string a_id, int a_order = 0)
	{
		return { std::move(a_id), SurfaceKind::Hud, false, false, a_order };
	}

	const MenuController::Layer* LayerOf(const std::vector<MenuController::Layer>& a_layers,
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
		MenuController mc;
		assert(!mc.Open("nope"));
		assert(!mc.Close("nope"));
		assert(!mc.CloseTop());
		assert(!mc.IsOpen("nope"));
		assert(!mc.IsRegistered("nope"));
		assert(!mc.DesiredVisible());
		assert(!mc.ActiveMenu());
	}

	// Single-menu policy: a second menu REPLACES the first, and reopening the
	// sole open menu reports no change.
	{
		MenuController mc;
		mc.Register(Menu("a/one"));
		mc.Register(Menu("a/two"));
		assert(mc.Open("a/one"));
		assert(!mc.Open("a/one"));  // already the sole open menu
		assert(mc.ActiveMenu() == std::optional<std::string>("a/one"));
		assert(mc.Open("a/two"));
		assert(mc.ActiveMenu() == std::optional<std::string>("a/two"));
		assert(!mc.IsOpen("a/one"));  // replaced, not stacked
		// The invariant Close()'s mid-vector erase relies on: the stack never
		// holds more than one entry, so erase == pop and no z-index reshuffle
		// can strand a hidden menu at a stale depth.
		assert(mc.DesiredLayers().size() == 2);
		int visibleMenus = 0;
		for (const auto& layer : mc.DesiredLayers()) {
			if (!layer.hidden) visibleMenus++;
		}
		assert(visibleMenus == 1);
	}

	// Derived policy follows the top-of-stack surface's flags.
	{
		MenuController mc;
		mc.Register(Menu("a/pausing", /*captures=*/true, /*pauses=*/true));
		mc.Register(Menu("a/passive", /*captures=*/false, /*pauses=*/false));
		assert(mc.Open("a/pausing"));
		assert(mc.DesiredCapture() && mc.DesiredPause() && mc.DesiredVisible());
		assert(mc.Open("a/passive"));
		assert(!mc.DesiredCapture() && !mc.DesiredPause() && mc.DesiredVisible());
		assert(mc.CloseTop());
		assert(!mc.DesiredVisible());
		assert(!mc.CloseTop());  // empty stack refuses
	}

	// HUDs: a shown set independent of the menu stack; menus sit above every
	// HUD in the composite z bands.
	{
		MenuController mc;
		mc.Register(Hud("a/hud", 5));
		mc.Register(Hud("a/hud2", 2000));  // order clamps into the HUD band
		mc.Register(Menu("a/menu"));
		assert(mc.Open("a/hud"));
		assert(!mc.Open("a/hud"));  // already shown
		assert(mc.DesiredVisible());
		assert(!mc.DesiredCapture());  // HUDs never capture
		assert(!mc.ActiveMenu());
		assert(mc.Open("a/hud2"));
		assert(mc.Open("a/menu"));
		const auto layers = mc.DesiredLayers();
		const auto* hud = LayerOf(layers, "a/hud");
		const auto* hud2 = LayerOf(layers, "a/hud2");
		const auto* menu = LayerOf(layers, "a/menu");
		assert(hud && !hud->hidden && hud->z == 5);
		assert(hud2 && !hud2->hidden && hud2->z == 999);  // clamped
		assert(menu && !menu->hidden && menu->z >= 1000);
		// Closing the menu leaves the HUDs shown; CloseTop touches menus only.
		assert(mc.CloseTop());
		assert(mc.IsOpen("a/hud") && mc.IsOpen("a/hud2"));
		assert(mc.DesiredVisible());
	}

	// Close() picks the right collection; CloseAll clears both.
	{
		MenuController mc;
		mc.Register(Hud("a/hud"));
		mc.Register(Menu("a/menu"));
		assert(mc.Open("a/hud"));
		assert(mc.Open("a/menu"));
		assert(mc.Close("a/menu"));
		assert(!mc.Close("a/menu"));  // already closed
		assert(mc.Close("a/hud"));
		assert(mc.Open("a/hud"));
		assert(mc.Open("a/menu"));
		mc.CloseAll();
		assert(!mc.DesiredVisible());
		assert(!mc.IsOpen("a/hud") && !mc.IsOpen("a/menu"));
		assert(mc.IsRegistered("a/hud") && mc.IsRegistered("a/menu"));
	}

	// Unregister closes first (true = policy must be re-applied) and makes the
	// id unopenable — the crash-recovery teardown path.
	{
		MenuController mc;
		mc.Register(Menu("a/menu"));
		assert(mc.Open("a/menu"));
		assert(mc.Unregister("a/menu"));   // was open: state changed
		assert(!mc.IsRegistered("a/menu"));
		assert(!mc.Open("a/menu"));
		mc.Register(Menu("a/menu"));
		assert(!mc.Unregister("a/menu"));  // closed: no state change
	}

	// Register is idempotent and replaces flags in place.
	{
		MenuController mc;
		mc.Register(Menu("a/menu", true, false));
		assert(mc.Open("a/menu"));
		mc.Register(Menu("a/menu", true, true));  // dev-reload flag change
		assert(mc.DesiredPause());
		assert(mc.IsOpen("a/menu"));  // replacement does not close it
	}

	std::cout << "menu controller tests passed\n";
}
