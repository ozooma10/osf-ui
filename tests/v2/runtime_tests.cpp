#include "v2/Runtime/ViewPresentationController.h"

#include <cassert>
#include <iostream>
#include <string>
#include <vector>

namespace
{
	Runtime::ViewManifest Menu(
		std::string a_id,
		bool a_capturesInput = true,
		bool a_pausesGame = true)
	{
		Runtime::ViewManifest view;
		view.id = std::move(a_id);
		view.kind = Runtime::ViewKind::Menu;
		view.capturesInput = a_capturesInput;
		view.pausesGame = a_pausesGame;
		return view;
	}

	Runtime::ViewManifest Hud(std::string a_id)
	{
		Runtime::ViewManifest view;
		view.id = std::move(a_id);
		view.kind = Runtime::ViewKind::Hud;
		view.capturesInput = false;
		view.pausesGame = false;
		return view;
	}

	void TestEmptyController()
	{
		Runtime::ViewPresentationController controller;

		assert(!controller.ActiveMenu());
		assert(!controller.IsOpen("author.mod/missing"));
		assert(!controller.CapturesInput());
		assert(!controller.PausesGame());
		assert(controller.OpenViewIds().empty());
	}

	void TestMenusReplaceEachOther()
	{
		Runtime::ViewPresentationController controller;

		const auto settings = Menu("osfui/settings", true, true);
		const auto keybindings = Menu("osfui/keybindings", false, false);

		assert(controller.Open(settings));
		assert(!controller.Open(settings));
		assert(controller.ActiveMenu() == std::optional<std::string>{ "osfui/settings" });
		assert(controller.CapturesInput());
		assert(controller.PausesGame());

		assert(controller.Open(keybindings));
		assert(!controller.IsOpen("osfui/settings"));
		assert(controller.IsOpen("osfui/keybindings"));
		assert(!controller.CapturesInput());
		assert(!controller.PausesGame());
	}

	void TestMultipleHuds()
	{
		Runtime::ViewPresentationController controller;

		const auto compass = Hud("author.mod/compass");
		const auto status = Hud("author.mod/status");

		assert(controller.Open(compass));
		assert(controller.Open(status));
		assert(!controller.Open(compass));
		assert(controller.IsOpen(compass.id));
		assert(controller.IsOpen(status.id));
		assert(!controller.ActiveMenu());
		assert(!controller.CapturesInput());
		assert(!controller.PausesGame());
	}

	void TestMenuAndHudsCoexist()
	{
		Runtime::ViewPresentationController controller;

		const auto hud = Hud("author.mod/status");
		const auto menu = Menu("osfui/settings");

		assert(controller.Open(hud));
		assert(controller.Open(menu));
		assert(controller.IsOpen(hud.id));
		assert(controller.IsOpen(menu.id));

		assert(controller.CloseActiveMenu());
		assert(controller.IsOpen(hud.id));
		assert(!controller.IsOpen(menu.id));
		assert(!controller.CloseActiveMenu());
	}

	void TestCloseAndCloseAll()
	{
		Runtime::ViewPresentationController controller;

		const auto firstHud = Hud("author.mod/first");
		const auto secondHud = Hud("author.mod/second");
		const auto menu = Menu("osfui/settings");

		controller.Open(firstHud);
		controller.Open(secondHud);
		controller.Open(menu);

		assert(controller.Close(firstHud.id));
		assert(!controller.Close(firstHud.id));
		assert(!controller.IsOpen(firstHud.id));

		controller.CloseAll();
		assert(controller.OpenViewIds().empty());
		assert(!controller.ActiveMenu());
		assert(!controller.CapturesInput());
		assert(!controller.PausesGame());
	}

	void TestOpenIdsAreSorted()
	{
		Runtime::ViewPresentationController controller;

		controller.Open(Hud("z.mod/status"));
		controller.Open(Hud("a.mod/compass"));
		controller.Open(Menu("osfui/settings"));

		const std::vector<std::string> expected{
			"a.mod/compass",
			"osfui/settings",
			"z.mod/status"
		};

		assert(controller.OpenViewIds() == expected);
	}
}

int main()
{
	TestEmptyController();
	TestMenusReplaceEachOther();
	TestMultipleHuds();
	TestMenuAndHudsCoexist();
	TestCloseAndCloseAll();
	TestOpenIdsAreSorted();

	std::cout << "v2 runtime tests passed\n";
	return 0;
}
