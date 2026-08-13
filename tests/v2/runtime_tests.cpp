#ifdef NDEBUG
#	undef NDEBUG
#endif
#include <cassert>
#include <chrono>
#include <fstream>
#include <iostream>
#include <string>
#include <vector>

#include "v2/Runtime/RuntimeCoordinator.h"
#include "v2/Runtime/ViewDiscovery.h"
#include "v2/Runtime/ViewPresentationController.h"
#include "v2/Runtime/ViewRuntime.h"

namespace
{
	int g_papyrusCalls = 0;
	int g_papyrusFailuresRemaining = 0;

	bool RegisterPapyrusForTest()
	{
		++g_papyrusCalls;

		if (g_papyrusFailuresRemaining > 0) {
			--g_papyrusFailuresRemaining;
			return false;
		}

		return true;
	}

	void ResetPapyrusTestState()
	{
		g_papyrusCalls = 0;
		g_papyrusFailuresRemaining = 0;
	}

	class ViewFixture
	{
	public:
		ViewFixture()
		{
			_root = std::filesystem::temp_directory_path() /
				("osfui-v2-views-" + std::to_string(
					std::chrono::steady_clock::now().time_since_epoch().count()));
			std::filesystem::create_directories(_root);
		}

		~ViewFixture()
		{
			std::error_code error;
			std::filesystem::remove_all(_root, error);
		}

		ViewFixture(const ViewFixture&) = delete;
		ViewFixture& operator=(const ViewFixture&) = delete;

		const std::filesystem::path& Root() const
		{
			return _root;
		}

		std::filesystem::path WriteManifest(
			std::string_view a_modId,
			std::string_view a_viewName,
			std::string_view a_json)
		{
			const auto directory = _root / a_modId / a_viewName;
			std::filesystem::create_directories(directory);

			const auto path = directory / "manifest.json";
			std::ofstream output{ path, std::ios::binary | std::ios::trunc };
			output << a_json;
			assert(output.good());
			return path;
		}

	private:
		std::filesystem::path _root;
	};

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

	void TestViewRuntimeResolvesCatalog()
	{
		Runtime::ViewRuntime runtime;
		runtime.ReplaceViews({
			Menu("osfui/settings", true, true),
			Hud("author.mod/compass")
		});

		const auto before = runtime.Presentation();

		assert(
			runtime.OpenView("author.mod/missing") ==
			Runtime::ViewOperationResult::UnknownView);
		assert(runtime.Presentation() == before);

		assert(
			runtime.OpenView("osfui/settings") ==
			Runtime::ViewOperationResult::Changed);

		const auto opened = runtime.Presentation();
		assert(opened.activeMenu == "osfui/settings");
		assert(opened.openViewIds ==
			std::vector<std::string>{ "osfui/settings" });
		assert(opened.capturesInput);
		assert(opened.pausesGame);

		assert(
			runtime.OpenView("osfui/settings") ==
			Runtime::ViewOperationResult::Unchanged);
	}

	void TestManifestKeepsUnknownKindFallback()
	{
		ViewFixture fixture;
		const auto path = fixture.WriteManifest(
			"author.mod",
			"fallback",
			R"({ "kind": "future-kind" })");

		const auto manifest = Runtime::LoadViewManifest(path);
		assert(manifest);
		assert(manifest->kind == Runtime::ViewKind::Menu);
		assert(manifest->capturesInput);
		assert(manifest->pausesGame);
	}

	void TestViewDiscoveryContainsInvalidNeighbors()
	{
		ViewFixture fixture;
		fixture.WriteManifest(
			"osfui",
			"settings",
			R"({ "title": "Mod Settings" })");
		fixture.WriteManifest(
			"author.mod",
			"compass",
			R"({
				"kind": "hud",
				"capturesInput": true,
				"pausesGame": true
			})");
		fixture.WriteManifest(
			"author.mod",
			"broken",
			R"({ "kind": )");
		fixture.WriteManifest(
			"author.mod",
			"unsafe",
			R"({ "entry": "../other-view/index.html" })");

		const auto result = Runtime::DiscoverViews(fixture.Root());

		assert(result.views.size() == 2);
		assert(result.views[0].id == "author.mod/compass");
		assert(result.views[1].id == "osfui/settings");
		assert(result.issues.size() == 2);

		const auto& hud = result.views[0];
		assert(hud.kind == Runtime::ViewKind::Hud);
		assert(!hud.capturesInput);
		assert(!hud.pausesGame);

		assert(std::ranges::any_of(result.issues, [](const auto& a_issue) {
			return a_issue.path.parent_path().filename() == "broken";
		}));
		assert(std::ranges::any_of(result.issues, [](const auto& a_issue) {
			return a_issue.path.parent_path().filename() == "unsafe";
		}));
	}

	void TestViewDiscoveryReportsMissingDirectory()
	{
		ViewFixture fixture;
		const auto result = Runtime::DiscoverViews(fixture.Root() / "missing");

		assert(result.views.empty());
		assert(result.issues.size() == 1);
		assert(result.issues[0].path == fixture.Root() / "missing");
	}

	void TestCoordinatorCoalescesDataLoadedWork()
	{
		ResetPapyrusTestState();
		Runtime::RuntimeCoordinator runtime{ &RegisterPapyrusForTest };

		runtime.Tick();
		assert(g_papyrusCalls == 0);

		runtime.NotifyDataLoaded();
		runtime.NotifyDataLoaded();
		runtime.Tick();
		assert(g_papyrusCalls == 1);

		runtime.Tick();
		assert(g_papyrusCalls == 1);

		runtime.NotifyDataLoaded();
		runtime.Tick();
		assert(g_papyrusCalls == 1);
	}

	void TestCoordinatorRetriesPapyrusRegistration()
	{
		ResetPapyrusTestState();
		g_papyrusFailuresRemaining = 1;
		Runtime::RuntimeCoordinator runtime{ &RegisterPapyrusForTest };

		runtime.NotifyDataLoaded();
		runtime.Tick();
		assert(g_papyrusCalls == 1);

		runtime.Tick();
		assert(g_papyrusCalls == 2);

		runtime.Tick();
		assert(g_papyrusCalls == 2);
	}

	void TestCoordinatorLoadsDiscoveredViews()
	{
		ViewFixture fixture;
		fixture.WriteManifest(
			"osfui",
			"settings",
			R"({ "title": "Mod Settings" })");
		fixture.WriteManifest(
			"author.mod",
			"broken",
			R"({ "kind": )");

		Runtime::RuntimeCoordinator runtime{ nullptr };
		const auto report = runtime.LoadViews(fixture.Root());

		assert(report.loaded == 1);
		assert(report.issues.size() == 1);

		const auto views = runtime.Views().Views();
		assert(views.size() == 1);
		assert(views[0].id == "osfui/settings");
		assert(
			runtime.Views().OpenView("osfui/settings") ==
			Runtime::ViewOperationResult::Changed);
	}

	void TestViewRuntimeQueuesPresentationCommands()
	{
		Runtime::ViewRuntime runtime;

		runtime.ReplaceViews({
			Menu("osfui/settings"),
			Menu("osfui/keybindings", false, false)
		});

		assert(runtime.TakePresentationCommands().empty());

		assert(
			runtime.OpenView("osfui/settings") ==
			Runtime::ViewOperationResult::Changed);

		auto commands = runtime.TakePresentationCommands();

		assert(commands.size() == 1);
		assert(
			commands[0].action ==
			Runtime::ViewPresentationAction::Show);
		assert(commands[0].view.id == "osfui/settings");

		// Taking commands drains the queue.
		assert(runtime.TakePresentationCommands().empty());

		// Reopening the same view creates no duplicate renderer work.
		assert(
			runtime.OpenView("osfui/settings") ==
			Runtime::ViewOperationResult::Unchanged);
		assert(runtime.TakePresentationCommands().empty());

		// Opening another menu hides the previous menu first.
		assert(
			runtime.OpenView("osfui/keybindings") ==
			Runtime::ViewOperationResult::Changed);

		commands = runtime.TakePresentationCommands();

		assert(commands.size() == 2);
		assert(
			commands[0].action ==
			Runtime::ViewPresentationAction::Hide);
		assert(commands[0].view.id == "osfui/settings");
		assert(
			commands[1].action ==
			Runtime::ViewPresentationAction::Show);
		assert(commands[1].view.id == "osfui/keybindings");

		assert(
			runtime.CloseView("osfui/keybindings") ==
			Runtime::ViewOperationResult::Changed);

		commands = runtime.TakePresentationCommands();

		assert(commands.size() == 1);
		assert(
			commands[0].action ==
			Runtime::ViewPresentationAction::Hide);
		assert(commands[0].view.id == "osfui/keybindings");
	}

	void TestReplacingViewsHidesOpenViews()
	{
		Runtime::ViewRuntime runtime;

		runtime.ReplaceViews({
			Menu("osfui/settings"),
			Hud("author.mod/status")
		});

		runtime.OpenView("osfui/settings");
		runtime.OpenView("author.mod/status");
		runtime.TakePresentationCommands();

		runtime.ReplaceViews({
			Menu("osfui/keybindings")
		});

		const auto commands = runtime.TakePresentationCommands();

		assert(commands.size() == 2);

		assert(
			commands[0].action ==
			Runtime::ViewPresentationAction::Hide);
		assert(commands[0].view.id == "author.mod/status");

		assert(
			commands[1].action ==
			Runtime::ViewPresentationAction::Hide);
		assert(commands[1].view.id == "osfui/settings");

		assert(runtime.Presentation().openViewIds.empty());
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
	TestViewRuntimeResolvesCatalog();
	TestViewRuntimeQueuesPresentationCommands();
	TestReplacingViewsHidesOpenViews();
	TestManifestKeepsUnknownKindFallback();
	TestViewDiscoveryContainsInvalidNeighbors();
	TestViewDiscoveryReportsMissingDirectory();
	TestCoordinatorCoalescesDataLoadedWork();
	TestCoordinatorRetriesPapyrusRegistration();
	TestCoordinatorLoadsDiscoveredViews();

	std::cout << "v2 runtime tests passed\n";
	return 0;
}
