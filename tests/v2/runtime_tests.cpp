#ifdef NDEBUG
#	undef NDEBUG
#endif
#include <cassert>
#include <chrono>
#include <fstream>
#include <initializer_list>
#include <iostream>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

#include "input/OverlayInputHook.h"
#include "v2/Runtime/RuntimeCoordinator.h"
#include "v2/Runtime/ViewDiscovery.h"
#include "v2/Runtime/ViewPresentationController.h"
#include "v2/Runtime/ViewRuntime.h"

#include "papyrus_tests.h"
#include "web_view_presenter_tests.h"

namespace
{
	int g_papyrusCalls = 0;
	int g_papyrusFailuresRemaining = 0;
	std::vector<bool> g_inputCaptureStates;

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

	void RecordInputCapture(bool a_captured)
	{
		g_inputCaptureStates.push_back(a_captured);
	}

	bool InputCapturedForTest()
	{
		return false;
	}

	bool HandleWindowKeyForTest(std::uint32_t, OSFUI::ScanCode, bool)
	{
		return false;
	}

	void HandleLayoutChangedForTest() {}
	void HandleMouseAbsoluteForTest(int, int, int, int) {}
	void HandleMouseButtonForTest(int, bool) {}
	void HandleMouseWheelForTest(int) {}

	struct PresentationCall
	{
		Runtime::ViewPresentationAction action;
		std::string viewId;
	};

	struct MousePositionCall
	{
		int clientX;
		int clientY;
		int clientWidth;
		int clientHeight;
	};

	struct MouseButtonCall
	{
		int button;
		bool down;
	};

	class RecordingViewPresenter final : public Runtime::IViewPresenter
	{
	public:
		bool Show(const Runtime::ViewManifest& a_view) noexcept override
		{
			calls.push_back({
				.action = Runtime::ViewPresentationAction::Show,
				.viewId = a_view.id
			});
			return showSucceeds;
		}

		void SetInputFocus(bool a_focused) noexcept override
		{
			inputFocusStates.push_back(a_focused);
		}

		void SendKeyEvent(std::uint32_t a_virtualKey, bool a_down) noexcept override
		{
			keyEvents.emplace_back(a_virtualKey, a_down);
		}

		void UpdateMousePosition(int a_clientX, int a_clientY, int a_clientWidth, int a_clientHeight) noexcept override
		{
			mousePositions.push_back({
				.clientX = a_clientX,
				.clientY = a_clientY,
				.clientWidth = a_clientWidth,
				.clientHeight = a_clientHeight
			});
		}

		void SendMouseButtonEvent(int a_button, bool a_down) noexcept override
		{
			mouseButtons.push_back({
				.button = a_button,
				.down = a_down
			});
		}

		void SendMouseWheelEvent(int a_wheelDelta) noexcept override
		{
			mouseWheelDeltas.push_back(a_wheelDelta);
		}

		void Hide(std::string_view a_viewId) noexcept override
		{
			calls.push_back({
				.action = Runtime::ViewPresentationAction::Hide,
				.viewId = std::string{ a_viewId }
			});
		}

		void Tick() noexcept override
		{
			++tickCalls;
			presentationCallCountsAtTick.push_back(calls.size());
		}

		bool showSucceeds{ true };
		std::size_t tickCalls{ 0 };
		std::vector<PresentationCall> calls;
		std::vector<bool> inputFocusStates;
		std::vector<std::pair<std::uint32_t, bool>> keyEvents;
		std::vector<MousePositionCall> mousePositions;
		std::vector<MouseButtonCall> mouseButtons;
		std::vector<int> mouseWheelDeltas;
		std::vector<std::size_t> presentationCallCountsAtTick;
	};

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

	void LoadCoordinatorViews(
		Runtime::RuntimeCoordinator& a_runtime,
		ViewFixture& a_fixture,
		std::initializer_list<std::string_view> a_viewIds)
	{
		for (const auto id : a_viewIds) {
			const auto separator = id.find('/');
			assert(separator != std::string_view::npos);

			a_fixture.WriteManifest(
				id.substr(0, separator),
				id.substr(separator + 1),
				R"({ "title": "Test View" })");
		}

		const auto report = a_runtime.LoadViews(a_fixture.Root());
		assert(report.loaded == a_viewIds.size());
		assert(report.issues.empty());
	}

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

	void TestGameWindowInputHandlersRequireCompleteTable()
	{
		using Handlers =
			OSFUI::OverlayInputHook::GameWindowInputHandlers;

		const Handlers complete{
			.isCaptured = &InputCapturedForTest,
			.onKey = &HandleWindowKeyForTest,
			.onKeyboardLayoutChanged = &HandleLayoutChangedForTest,
			.onMouseAbsolute = &HandleMouseAbsoluteForTest,
			.onMouseButton = &HandleMouseButtonForTest,
			.onMouseWheel = &HandleMouseWheelForTest
		};

		assert(complete.IsComplete());

		auto missing = complete;
		missing.isCaptured = nullptr;
		assert(!missing.IsComplete());
		missing = complete;
		missing.onKey = nullptr;
		assert(!missing.IsComplete());
		missing = complete;
		missing.onKeyboardLayoutChanged = nullptr;
		assert(!missing.IsComplete());
		missing = complete;
		missing.onMouseAbsolute = nullptr;
		assert(!missing.IsComplete());
		missing = complete;
		missing.onMouseButton = nullptr;
		assert(!missing.IsComplete());
		missing = complete;
		missing.onMouseWheel = nullptr;
		assert(!missing.IsComplete());
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

	void TestCoordinatorDispatchesPresentationAlongsideLifecycle()
	{
		ResetPapyrusTestState();
		RecordingViewPresenter presenter;
		Runtime::RuntimeCoordinator runtime{
			&RegisterPapyrusForTest,
			&presenter
		};

		runtime.Views().ReplaceViews({
			Menu("osfui/settings")
		});
		runtime.NotifyDataLoaded();
		assert(
			runtime.Views().OpenView("osfui/settings") ==
			Runtime::ViewOperationResult::Changed);

		runtime.Tick();

		assert(g_papyrusCalls == 1);
		assert(presenter.tickCalls == 1);
		assert(presenter.presentationCallCountsAtTick[0] == 1);
		assert(presenter.calls.size() == 1);
		assert(
			presenter.calls[0].action ==
			Runtime::ViewPresentationAction::Show);
		assert(presenter.calls[0].viewId == "osfui/settings");

		presenter.calls.clear();
		assert(
			runtime.Views().CloseView("osfui/settings") ==
			Runtime::ViewOperationResult::Changed);

		// Presentation still runs after Papyrus registration is complete.
		runtime.Tick();

		assert(g_papyrusCalls == 1);
		assert(presenter.tickCalls == 2);
		assert(presenter.presentationCallCountsAtTick[1] == 1);
		assert(presenter.calls.size() == 1);
		assert(
			presenter.calls[0].action ==
			Runtime::ViewPresentationAction::Hide);
		assert(presenter.calls[0].viewId == "osfui/settings");

		presenter.calls.clear();
		runtime.Tick();
		assert(presenter.tickCalls == 3);
		assert(presenter.presentationCallCountsAtTick[2] == 0);
		assert(presenter.calls.empty());
	}

	void TestCoordinatorTicksPresenterWithoutCommands()
	{
		RecordingViewPresenter presenter;
		Runtime::RuntimeCoordinator runtime{ nullptr, &presenter };

		runtime.Tick();
		runtime.Tick();

		assert(presenter.tickCalls == 2);
		assert(presenter.calls.empty());
		assert((presenter.presentationCallCountsAtTick ==
			std::vector<std::size_t>{ 0, 0 }));
	}

	void TestCoordinatorDispatchesMenuReplacementInOrder()
	{
		RecordingViewPresenter presenter;
		Runtime::RuntimeCoordinator runtime{ nullptr, &presenter };

		runtime.Views().ReplaceViews({
			Menu("osfui/settings"),
			Menu("osfui/keybindings")
		});

		runtime.Views().OpenView("osfui/settings");
		runtime.Tick();
		presenter.calls.clear();

		runtime.Views().OpenView("osfui/keybindings");
		runtime.Tick();

		assert(presenter.calls.size() == 2);
		assert(
			presenter.calls[0].action ==
			Runtime::ViewPresentationAction::Hide);
		assert(presenter.calls[0].viewId == "osfui/settings");
		assert(
			presenter.calls[1].action ==
			Runtime::ViewPresentationAction::Show);
		assert(presenter.calls[1].viewId == "osfui/keybindings");
	}

	void TestCoordinatorDefersViewRequestsUntilTick()
	{
		ViewFixture fixture;
		RecordingViewPresenter presenter;
		Runtime::RuntimeCoordinator runtime{ nullptr, &presenter };

		LoadCoordinatorViews(runtime, fixture, { "osfui/settings" });

		assert(runtime.RequestOpenView("osfui/settings"));
		assert(runtime.Views().Presentation().openViewIds.empty());
		assert(presenter.calls.empty());

		runtime.Tick();

		const auto opened = runtime.Views().Presentation();
		assert(opened.activeMenu == "osfui/settings");
		assert((opened.openViewIds ==
			std::vector<std::string>{ "osfui/settings" }));
		assert(presenter.calls.size() == 1);
		assert(presenter.calls[0].action ==
			Runtime::ViewPresentationAction::Show);
		assert(presenter.calls[0].viewId == "osfui/settings");
		assert(presenter.presentationCallCountsAtTick[0] == 1);

		presenter.calls.clear();
		assert(runtime.RequestCloseView("osfui/settings"));
		assert(runtime.Views().Presentation().activeMenu == "osfui/settings");

		runtime.Tick();

		const auto closed = runtime.Views().Presentation();
		assert(!closed.activeMenu);
		assert(closed.openViewIds.empty());
		assert(presenter.calls.size() == 1);
		assert(presenter.calls[0].action ==
			Runtime::ViewPresentationAction::Hide);
		assert(presenter.calls[0].viewId == "osfui/settings");
	}

	void TestCoordinatorAppliesViewRequestsInFifoOrder()
	{
		ViewFixture fixture;
		RecordingViewPresenter presenter;
		Runtime::RuntimeCoordinator runtime{ nullptr, &presenter };

		LoadCoordinatorViews(runtime, fixture, {
			"osfui/settings",
			"osfui/keybindings"
		});

		assert(runtime.RequestOpenView("osfui/settings"));
		assert(runtime.RequestOpenView("osfui/keybindings"));
		assert(!runtime.RequestCloseView("osfui/keybindings"));

		runtime.Tick();

		assert(presenter.calls.size() == 3);
		assert(presenter.calls[0].action ==
			Runtime::ViewPresentationAction::Show);
		assert(presenter.calls[0].viewId == "osfui/settings");
		assert(presenter.calls[1].action ==
			Runtime::ViewPresentationAction::Hide);
		assert(presenter.calls[1].viewId == "osfui/settings");
		assert(presenter.calls[2].action ==
			Runtime::ViewPresentationAction::Show);
		assert(presenter.calls[2].viewId == "osfui/keybindings");
		assert(runtime.Views().Presentation().activeMenu ==
			"osfui/keybindings");

		presenter.calls.clear();
		assert(runtime.RequestCloseView("osfui/keybindings"));
		runtime.Tick();

		assert(presenter.calls.size() == 1);
		assert(presenter.calls[0].action ==
			Runtime::ViewPresentationAction::Hide);
		assert(presenter.calls[0].viewId == "osfui/keybindings");
		assert(runtime.Views().Presentation().openViewIds.empty());
	}

	void TestCoordinatorContainsInvalidViewRequests()
	{
		ViewFixture fixture;
		RecordingViewPresenter presenter;
		Runtime::RuntimeCoordinator runtime{ nullptr, &presenter };

		LoadCoordinatorViews(runtime, fixture, { "osfui/settings" });

		assert(!runtime.RequestOpenView(""));
		assert(!runtime.RequestCloseView(""));
		assert(!runtime.RequestOpenView("author.mod/missing"));
		assert(!runtime.RequestCloseView("author.mod/missing"));
		assert(!runtime.RequestCloseView("osfui/settings"));

		runtime.Tick();

		assert(runtime.Views().Presentation().openViewIds.empty());
		assert(presenter.calls.empty());
		assert(presenter.tickCalls == 1);
	}

	void TestCoordinatorBoundsConcurrentViewRequests()
	{
		ViewFixture fixture;
		RecordingViewPresenter presenter;
		Runtime::RuntimeCoordinator runtime{ nullptr, &presenter };

		LoadCoordinatorViews(runtime, fixture, { "osfui/settings" });

		constexpr std::size_t threadCount = 4;
		constexpr std::size_t requestsPerThread = 64;
		std::atomic_size_t accepted{ 0 };
		std::vector<std::thread> threads;

		for (std::size_t thread = 0; thread < threadCount; ++thread) {
			threads.emplace_back([&] {
				for (std::size_t request = 0; request < requestsPerThread; ++request) {
					if (runtime.RequestOpenView("osfui/settings")) {
						accepted.fetch_add(1, std::memory_order_relaxed);
					}
				}
			});
		}

		for (auto& thread : threads) {
			thread.join();
		}

		assert(accepted.load(std::memory_order_relaxed) == 256);
		assert(!runtime.RequestOpenView("osfui/settings"));

		runtime.Tick();

		// Duplicate opens collapse in ViewRuntime, so the bounded batch produces
		// exactly one presenter transition.
		assert(presenter.calls.size() == 1);
		assert(presenter.calls[0].action ==
			Runtime::ViewPresentationAction::Show);

		// Draining releases the complete capacity for the following tick.
		assert(runtime.RequestCloseView("osfui/settings"));
		presenter.calls.clear();
		runtime.Tick();
		assert(presenter.calls.size() == 1);
		assert(presenter.calls[0].action ==
			Runtime::ViewPresentationAction::Hide);
	}

	void TestCoordinatorTracksOnlySuccessfulInstantiation()
	{
		ViewFixture fixture;
		RecordingViewPresenter presenter;
		Runtime::RuntimeCoordinator runtime{ nullptr, &presenter };

		LoadCoordinatorViews(runtime, fixture, { "osfui/settings" });

		assert(!runtime.RequestCloseView("osfui/settings"));

		presenter.showSucceeds = false;
		assert(runtime.RequestOpenView("osfui/settings"));
		runtime.Tick();

		assert(!runtime.RequestCloseView("osfui/settings"));
		assert(runtime.Views().Presentation().openViewIds.empty());

		presenter.showSucceeds = true;
		presenter.calls.clear();
		assert(runtime.RequestOpenView("osfui/settings"));
		runtime.Tick();
		assert(presenter.calls.size() == 1);

		assert(runtime.RequestCloseView("osfui/settings"));
		runtime.Tick();
		assert(runtime.Views().Presentation().openViewIds.empty());

		// Hiding preserves the document instance, so it remains reopenable.
		presenter.calls.clear();
		assert(runtime.RequestOpenView("osfui/settings"));
		runtime.Tick();
		assert(presenter.calls.size() == 1);
		assert(presenter.calls[0].action ==
			Runtime::ViewPresentationAction::Show);
	}

	void TestCoordinatorClosesViewWhenPresentationFails()
	{
		RecordingViewPresenter presenter;
		presenter.showSucceeds = false;
		Runtime::RuntimeCoordinator runtime{ nullptr, &presenter };

		runtime.Views().ReplaceViews({
			Menu("osfui/settings")
		});
		runtime.Views().OpenView("osfui/settings");

		runtime.Tick();

		assert(presenter.calls.size() == 2);
		assert(
			presenter.calls[0].action ==
			Runtime::ViewPresentationAction::Show);
		assert(presenter.calls[0].viewId == "osfui/settings");
		assert(
			presenter.calls[1].action ==
			Runtime::ViewPresentationAction::Hide);
		assert(presenter.calls[1].viewId == "osfui/settings");

		const auto state = runtime.Views().Presentation();
		assert(state.openViewIds.empty());
		assert(!state.activeMenu);
		assert(!state.capturesInput);
		assert(!state.pausesGame);

		presenter.calls.clear();
		runtime.Tick();
		assert(presenter.calls.empty());
	}

	void TestCoordinatorReconcilesInputFocusAcrossMenuAndHudChanges()
	{
		RecordingViewPresenter presenter;
		Runtime::RuntimeCoordinator runtime{ nullptr, &presenter };

		runtime.Views().ReplaceViews({
			Menu("osfui/settings"),
			Hud("author.mod/status")
		});

		runtime.Views().OpenView("osfui/settings");
		runtime.Tick();
		assert((presenter.inputFocusStates == std::vector<bool>{ true }));

		// A steady tick and a HUD opening do not create redundant focus edges.
		runtime.Tick();
		runtime.Views().OpenView("author.mod/status");
		runtime.Tick();
		assert((presenter.inputFocusStates == std::vector<bool>{ true }));

		// The menu releases focus even though the HUD remains visible.
		runtime.Views().CloseView("osfui/settings");
		runtime.Tick();
		assert((presenter.inputFocusStates ==
			std::vector<bool>{ true, false }));
		assert((runtime.Views().Presentation().openViewIds ==
			std::vector<std::string>{ "author.mod/status" }));
	}

	void TestCoordinatorAppliesInputCapturePolicyEveryTick()
	{
		g_inputCaptureStates.clear();
		RecordingViewPresenter presenter;
		Runtime::RuntimeCoordinator runtime{
			nullptr,
			&presenter,
			&RecordInputCapture
		};

		runtime.Views().ReplaceViews({
			Menu("osfui/settings"),
			Hud("author.mod/status")
		});

		runtime.Views().OpenView("osfui/settings");
		runtime.Tick();
		runtime.Tick();

		// The production ControlLayer owns edge detection and startup retry, so
		// the desired capture state must be supplied on every tick.
		assert((g_inputCaptureStates == std::vector<bool>{ true, true }));

		runtime.Views().OpenView("author.mod/status");
		runtime.Tick();
		assert((g_inputCaptureStates ==
			std::vector<bool>{ true, true, true }));

		runtime.Views().CloseView("osfui/settings");
		runtime.Tick();
		assert((g_inputCaptureStates ==
			std::vector<bool>{ true, true, true, false }));
	}

	void TestCoordinatorReleasesInputOwnershipForNonCapturingReplacement()
	{
		g_inputCaptureStates.clear();
		RecordingViewPresenter presenter;
		Runtime::RuntimeCoordinator runtime{
			nullptr,
			&presenter,
			&RecordInputCapture
		};

		runtime.Views().ReplaceViews({
			Menu("osfui/settings", true),
			Menu("author.mod/dashboard", false)
		});

		runtime.Views().OpenView("osfui/settings");
		runtime.Tick();
		runtime.Views().OpenView("author.mod/dashboard");
		runtime.Tick();

		assert((presenter.inputFocusStates ==
			std::vector<bool>{ true, false }));
		assert((g_inputCaptureStates ==
			std::vector<bool>{ true, false }));
		assert(runtime.Views().Presentation().activeMenu ==
			"author.mod/dashboard");
	}

	void TestCoordinatorDoesNotCaptureFailedPresentation()
	{
		g_inputCaptureStates.clear();
		RecordingViewPresenter presenter;
		presenter.showSucceeds = false;
		Runtime::RuntimeCoordinator runtime{
			nullptr,
			&presenter,
			&RecordInputCapture
		};

		runtime.Views().ReplaceViews({
			Menu("osfui/settings")
		});
		runtime.Views().OpenView("osfui/settings");

		runtime.Tick();

		assert(presenter.inputFocusStates.empty());
		assert((g_inputCaptureStates == std::vector<bool>{ false }));
		assert(runtime.Views().Presentation().openViewIds.empty());
	}

	void TestCoordinatorNeverCapturesWithoutPresenter()
	{
		g_inputCaptureStates.clear();
		Runtime::RuntimeCoordinator runtime{
			nullptr,
			nullptr,
			&RecordInputCapture
		};

		runtime.Views().ReplaceViews({
			Menu("osfui/settings")
		});
		runtime.Views().OpenView("osfui/settings");
		runtime.Tick();

		assert((g_inputCaptureStates == std::vector<bool>{ false }));
	}

	void TestCoordinatorRoutesKeyboardWhileCaptured()
	{
		RecordingViewPresenter presenter;
		Runtime::RuntimeCoordinator runtime{ nullptr, &presenter };

		runtime.Views().ReplaceViews({
			Menu("osfui/settings")
		});
		runtime.Views().OpenView("osfui/settings");
		runtime.Tick();

		assert(runtime.IsInputCaptured());
		assert(runtime.RouteKeyEvent(0x41, true));
		assert(runtime.RouteKeyEvent(0x41, false));
		assert((presenter.keyEvents ==
			std::vector<std::pair<std::uint32_t, bool>>{
				{ 0x41, true },
				{ 0x41, false }
			}));
	}

	void TestCoordinatorClosesActiveMenuForEscape()
	{
		RecordingViewPresenter presenter;
		Runtime::RuntimeCoordinator runtime{ nullptr, &presenter };

		runtime.Views().ReplaceViews({
			Menu("osfui/settings")
		});
		runtime.Views().OpenView("osfui/settings");
		runtime.Tick();

		assert(runtime.RouteKeyEvent(0x1B, true));
		assert(runtime.RouteKeyEvent(0x1B, false));
		assert(presenter.keyEvents.empty());
		assert(runtime.Views().Presentation().activeMenu == "osfui/settings");

		runtime.Tick();

		assert(!runtime.Views().Presentation().activeMenu);
		assert(!runtime.IsInputCaptured());
		assert(presenter.calls.back().action == Runtime::ViewPresentationAction::Hide);
		assert(presenter.calls.back().viewId == "osfui/settings");
	}

	void TestCoordinatorDoesNotRouteInputToPassiveHud()
	{
		RecordingViewPresenter presenter;
		Runtime::RuntimeCoordinator runtime{ nullptr, &presenter };

		runtime.Views().ReplaceViews({
			Hud("author.mod/status")
		});
		runtime.Views().OpenView("author.mod/status");
		runtime.Tick();

		assert(!runtime.IsInputCaptured());
		assert(!runtime.RouteKeyEvent(0x41, true));
		runtime.RouteMousePosition(100, 200, 1920, 1080);
		runtime.RouteMouseButtonEvent(0, true);
		runtime.RouteMouseWheelEvent(120);

		assert(presenter.keyEvents.empty());
		assert(presenter.mousePositions.empty());
		assert(presenter.mouseButtons.empty());
		assert(presenter.mouseWheelDeltas.empty());
	}

	void TestCoordinatorStopsRoutingAfterMenuClose()
	{
		RecordingViewPresenter presenter;
		Runtime::RuntimeCoordinator runtime{ nullptr, &presenter };

		runtime.Views().ReplaceViews({
			Menu("osfui/settings")
		});
		runtime.Views().OpenView("osfui/settings");
		runtime.Tick();

		assert(runtime.RouteKeyEvent(0x41, true));
		runtime.RouteMousePosition(100, 200, 1920, 1080);
		runtime.RouteMouseButtonEvent(0, true);
		runtime.RouteMouseWheelEvent(120);

		assert(runtime.RequestCloseView("osfui/settings"));
		runtime.Tick();
		assert(!runtime.IsInputCaptured());

		const auto keyCount = presenter.keyEvents.size();
		const auto positionCount = presenter.mousePositions.size();
		const auto buttonCount = presenter.mouseButtons.size();
		const auto wheelCount = presenter.mouseWheelDeltas.size();

		assert(!runtime.RouteKeyEvent(0x42, true));
		runtime.RouteMousePosition(300, 400, 1920, 1080);
		runtime.RouteMouseButtonEvent(0, false);
		runtime.RouteMouseWheelEvent(-120);

		assert(presenter.keyEvents.size() == keyCount);
		assert(presenter.mousePositions.size() == positionCount);
		assert(presenter.mouseButtons.size() == buttonCount);
		assert(presenter.mouseWheelDeltas.size() == wheelCount);
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
	TestGameWindowInputHandlersRequireCompleteTable();
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
	TestCoordinatorDispatchesPresentationAlongsideLifecycle();
	TestCoordinatorTicksPresenterWithoutCommands();
	TestCoordinatorDispatchesMenuReplacementInOrder();
	TestCoordinatorDefersViewRequestsUntilTick();
	TestCoordinatorAppliesViewRequestsInFifoOrder();
	TestCoordinatorContainsInvalidViewRequests();
	TestCoordinatorBoundsConcurrentViewRequests();
	TestCoordinatorTracksOnlySuccessfulInstantiation();
	TestCoordinatorClosesViewWhenPresentationFails();
	TestCoordinatorReconcilesInputFocusAcrossMenuAndHudChanges();
	TestCoordinatorAppliesInputCapturePolicyEveryTick();
	TestCoordinatorReleasesInputOwnershipForNonCapturingReplacement();
	TestCoordinatorDoesNotCaptureFailedPresentation();
	TestCoordinatorNeverCapturesWithoutPresenter();
	TestCoordinatorRoutesKeyboardWhileCaptured();
	TestCoordinatorClosesActiveMenuForEscape();
	TestCoordinatorDoesNotRouteInputToPassiveHud();
	TestCoordinatorStopsRoutingAfterMenuClose();
	RunPapyrusTests();
	RunWebViewPresenterTests();

	std::cout << "v2 runtime tests passed\n";
	return 0;
}
