#ifdef NDEBUG
#	undef NDEBUG
#endif
#include <cassert>
#include <stdexcept>

#include "composite/ICompositor.h"
#include "v2/Presentation/WebViewPresenter.h"

namespace
{
	bool g_drawAvailable = false;
	std::vector<std::pair<std::uint32_t, bool>> g_frameworkKeyEvents;

	bool DrawAvailableForTest()
	{
		return g_drawAvailable;
	}

	bool HandleFrameworkKeyForTest(std::uint32_t a_virtualKey, bool a_down)
	{
		g_frameworkKeyEvents.emplace_back(a_virtualKey, a_down);
		return a_virtualKey == 0x1B;
	}

	struct AcceleratorState
	{
		std::uint32_t toggleScan;
		bool captured;
		bool captureArmed;
		std::uint32_t captureUpScan;
	};

	struct MouseButtonEvent
	{
		int x;
		int y;
		int button;
		bool down;
	};

	struct MouseWheelEvent
	{
		int x;
		int y;
		int wheelDelta;
	};

	class FakeWebRenderer final : public OSFUI::IWebRenderer
	{
	public:
		explicit FakeWebRenderer(std::vector<std::string>* a_destructionLog = nullptr) :
			_destructionLog(a_destructionLog)
		{}

		~FakeWebRenderer() override
		{
			if (_destructionLog) {
				_destructionLog->push_back("renderer");
			}
		}

		bool Initialize(const OSFUI::RendererConfig& a_config) override
		{
			++initializeCalls;
			config = a_config;
			return initializeSucceeds;
		}

		void CreateOrNavigateView(const OSFUI::ViewManifest& a_manifest) override
		{
			createdViews.push_back(a_manifest);
		}

		void SetInputTargetView(std::string_view a_id) override
		{
			inputTargets.emplace_back(a_id);
		}

		void Resize(std::uint32_t a_width, std::uint32_t a_height) override
		{
			resizeCalls.emplace_back(a_width, a_height);
		}

		void Update(double a_deltaSeconds) override
		{
			updateDeltas.push_back(a_deltaSeconds);
			if (throwOnUpdate) {
				throw std::runtime_error{ "update failed" };
			}
		}

		std::optional<OSFUI::FrameBufferView> Render() override
		{
			return frame;
		}

		void SendMessageToWeb(std::string_view, std::string_view) override
		{}

		void SetSharedRingHandler(SharedRingHandler a_handler) override
		{
			ringHandler = std::move(a_handler);
		}

		void SetNativeAcceleratorHandler(NativeAcceleratorHandler a_handler) override
		{
			nativeAcceleratorHandler = std::move(a_handler);
		}

		void SetNativeFocus(bool a_focused) override
		{
			inputStateTransitions.push_back(a_focused ? "focus:on" : "focus:off");
			if (throwOnNativeFocus) {
				throw std::runtime_error{ "native focus failed" };
			}
			nativeFocusStates.push_back(a_focused);
		}

		void SetAcceleratorKeys(std::uint32_t a_toggleScan, bool a_captured, bool a_captureArmed, std::uint32_t a_captureUpScan) override
		{
			inputStateTransitions.push_back(a_captured ? "accelerators:on" : "accelerators:off");
			acceleratorStates.push_back({
				.toggleScan = a_toggleScan,
				.captured = a_captured,
				.captureArmed = a_captureArmed,
				.captureUpScan = a_captureUpScan
			});
		}

		void InjectKeyEvent(std::uint32_t a_virtualKey, bool a_down) override
		{
			keyEvents.emplace_back(a_virtualKey, a_down);
		}

		void InjectMouseMove(int a_x, int a_y) override
		{
			mouseMoves.emplace_back(a_x, a_y);
		}

		void InjectMouseButton(int a_x, int a_y, int a_button, bool a_down) override
		{
			mouseButtons.push_back({
				.x = a_x,
				.y = a_y,
				.button = a_button,
				.down = a_down
			});
		}

		void InjectPhysicalMouseWheel(int a_x, int a_y, int a_wheelDelta) override
		{
			physicalMouseWheels.push_back({
				.x = a_x,
				.y = a_y,
				.wheelDelta = a_wheelDelta
			});
		}

		void SetViewHidden(std::string_view a_viewId, bool a_hidden) override
		{
			hiddenChanges.emplace_back(std::string{ a_viewId }, a_hidden);
		}

		std::string_view Name() const override
		{
			return "fake-renderer";
		}

		void EmitSharedRing(const OSFUI::SharedRingDesc& a_ring)
		{
			assert(ringHandler);
			ringHandler(a_ring);
		}

		bool EmitNativeAccelerator(std::uint32_t a_virtualKey, std::uint32_t a_scanCode, bool a_down)
		{
			assert(nativeAcceleratorHandler);
			return nativeAcceleratorHandler(a_virtualKey, a_scanCode, a_down);
		}

		bool initializeSucceeds{ true };
		bool throwOnUpdate{ false };
		bool throwOnNativeFocus{ false };
		int initializeCalls{ 0 };
		std::optional<OSFUI::RendererConfig> config;
		std::vector<OSFUI::ViewManifest> createdViews;
		std::vector<std::string> inputTargets;
		std::vector<std::pair<std::uint32_t, std::uint32_t>> resizeCalls;
		std::vector<double> updateDeltas;
		std::optional<OSFUI::FrameBufferView> frame;
		SharedRingHandler ringHandler;
		NativeAcceleratorHandler nativeAcceleratorHandler;
		std::vector<AcceleratorState> acceleratorStates;
		std::vector<std::string> inputStateTransitions;
		std::vector<bool> nativeFocusStates;
		std::vector<std::pair<std::uint32_t, bool>> keyEvents;
		std::vector<std::pair<int, int>> mouseMoves;
		std::vector<MouseButtonEvent> mouseButtons;
		std::vector<MouseWheelEvent> physicalMouseWheels;
		std::vector<std::pair<std::string, bool>> hiddenChanges;

	private:
		std::vector<std::string>* _destructionLog;
	};

	class FakeCompositor final : public OSFUI::ICompositor
	{
	public:
		explicit FakeCompositor(std::vector<std::string>* a_destructionLog = nullptr) :
			_destructionLog(a_destructionLog)
		{}

		~FakeCompositor() override
		{
			if (_destructionLog) {
				_destructionLog->push_back("compositor");
			}
		}

		bool Initialize() override
		{
			++initializeCalls;
			return initializeSucceeds;
		}

		void Submit(const OSFUI::FrameBufferView& a_frame) override
		{
			submittedFrames.push_back(a_frame);
		}

		void SetVisible(bool a_visible) override
		{
			if (throwOnSetVisible) {
				throw std::runtime_error{ "visibility failed" };
			}
			visibleStates.push_back(a_visible);
		}

		void SetOutputResizeCallback(OutputResizeCallback a_callback) override
		{
			resizeCallback = std::move(a_callback);
		}

		void SetSharedRing(const OSFUI::SharedRingDesc& a_ring) override
		{
			lastRingGeneration = a_ring.generation;
		}

		void SetScaleformOverlayEnabled(bool a_enabled) override
		{
			scaleformOverlayStates.push_back(a_enabled);
		}

		std::string_view Name() const override
		{
			return "fake-compositor";
		}

		void EmitResize(std::uint32_t a_width, std::uint32_t a_height)
		{
			assert(resizeCallback);
			resizeCallback(a_width, a_height);
		}

		bool initializeSucceeds{ true };
		bool throwOnSetVisible{ false };
		int initializeCalls{ 0 };
		std::vector<OSFUI::FrameBufferView> submittedFrames;
		std::vector<bool> visibleStates;
		OutputResizeCallback resizeCallback;
		std::uint64_t lastRingGeneration{ 0 };
		std::vector<bool> scaleformOverlayStates;

	private:
		std::vector<std::string>* _destructionLog;
	};

	Runtime::ViewManifest MenuView()
	{
		Runtime::ViewManifest view;
		view.id = "osfui/settings";
		view.title = "Mod Settings";
		view.entry = "settings.html";
		view.width = 1920;
		view.height = 1080;
		view.transparent = false;
		view.kind = Runtime::ViewKind::Menu;
		view.capturesInput = true;
		view.pausesGame = false;
		view.openOnStart = true;
		view.rootDirectory = "views/osfui/settings";
		return view;
	}

	void TestPresenterInitializationAndTransportWiring()
	{
		g_frameworkKeyEvents.clear();
		auto renderer = std::make_unique<FakeWebRenderer>();
		auto compositor = std::make_unique<FakeCompositor>();
		auto* rendererPtr = renderer.get();
		auto* compositorPtr = compositor.get();

		Presentation::WebViewPresenter presenter{
			std::move(renderer),
			std::move(compositor),
			&DrawAvailableForTest,
			&HandleFrameworkKeyForTest
		};

		const std::filesystem::path dataDirectory{ "test-data/OSFUI" };
		assert(presenter.Initialize(dataDirectory));
		assert(presenter.Initialize(dataDirectory));

		assert(rendererPtr->initializeCalls == 1);
		assert(rendererPtr->config);
		assert(rendererPtr->config->dataDir == dataDirectory);
		assert(rendererPtr->config->width == OSFUI::kDefaultViewWidth);
		assert(rendererPtr->config->height == OSFUI::kDefaultViewHeight);
		assert(compositorPtr->initializeCalls == 1);
		assert(compositorPtr->visibleStates == std::vector<bool>{ false });
		assert(rendererPtr->EmitNativeAccelerator(0x1B, 0x01, true));
		assert((g_frameworkKeyEvents ==
			std::vector<std::pair<std::uint32_t, bool>>{
				{ 0x1B, true }
			}));

		compositorPtr->EmitResize(2560, 1440);
		assert(rendererPtr->resizeCalls ==
			(std::vector<std::pair<std::uint32_t, std::uint32_t>>{
				{ 2560, 1440 }
			}));

		OSFUI::SharedRingDesc ring;
		ring.generation = 17;
		rendererPtr->EmitSharedRing(ring);
		assert(compositorPtr->lastRingGeneration == 17);
	}

	void TestPresenterShowHideAndManifestConversion()
	{
		g_drawAvailable = false;

		auto renderer = std::make_unique<FakeWebRenderer>();
		auto compositor = std::make_unique<FakeCompositor>();
		auto* rendererPtr = renderer.get();
		auto* compositorPtr = compositor.get();

		Presentation::WebViewPresenter presenter{
			std::move(renderer),
			std::move(compositor),
			&DrawAvailableForTest,
			&HandleFrameworkKeyForTest
		};

		assert(presenter.Initialize("test-data/OSFUI"));
		presenter.SetDrawPathInstalled(true);
		assert(compositorPtr->scaleformOverlayStates == std::vector<bool>{ true });

		const auto view = MenuView();
		assert(!presenter.Show(view));
		assert(rendererPtr->createdViews.empty());

		g_drawAvailable = true;
		assert(presenter.Show(view));
		assert(rendererPtr->createdViews.size() == 1);

		const auto& converted = rendererPtr->createdViews[0];
		assert(converted.id == view.id);
		assert(converted.title == view.title);
		assert(converted.entry == view.entry);
		assert(converted.width == view.width);
		assert(converted.height == view.height);
		assert(converted.transparent == view.transparent);
		assert(converted.kind == OSFUI::ViewKind::Menu);
		assert(converted.menuInputEligible);
		assert(converted.capturesInput == view.capturesInput);
		assert(converted.pausesGame == view.pausesGame);
		assert(converted.openOnStart == view.openOnStart);
		assert(converted.rootDir == view.rootDirectory);

		assert(rendererPtr->hiddenChanges.back() ==
			(std::pair<std::string, bool>{ view.id, false }));
		assert(rendererPtr->inputTargets.back() == view.id);
		assert(compositorPtr->visibleStates.back());

		assert(presenter.Show(view));
		assert(rendererPtr->createdViews.size() == 1);

		presenter.SetInputFocus(true);
		assert(rendererPtr->nativeFocusStates == std::vector<bool>{ true });
		assert((rendererPtr->inputStateTransitions ==
			std::vector<std::string>{ "accelerators:on", "focus:on" }));
		assert(rendererPtr->acceleratorStates.size() == 1);
		assert(rendererPtr->acceleratorStates[0].toggleScan == 0);
		assert(rendererPtr->acceleratorStates[0].captured);
		assert(!rendererPtr->acceleratorStates[0].captureArmed);
		assert(rendererPtr->acceleratorStates[0].captureUpScan == 0);

		presenter.Hide(view.id);
		assert(rendererPtr->hiddenChanges.back() ==
			(std::pair<std::string, bool>{ view.id, true }));
		assert(rendererPtr->nativeFocusStates == std::vector<bool>{ true });
		assert(!compositorPtr->visibleStates.back());

		presenter.SetInputFocus(false);
		assert((rendererPtr->nativeFocusStates ==
			std::vector<bool>{ true, false }));
		assert((rendererPtr->inputStateTransitions ==
			std::vector<std::string>{ "accelerators:on", "focus:on", "focus:off", "accelerators:off" }));
		assert(rendererPtr->acceleratorStates.size() == 2);
		assert(!rendererPtr->acceleratorStates[1].captured);
	}

	void TestPresenterTickSubmitsFramesAndTracksDrawAvailability()
	{
		g_drawAvailable = true;

		auto renderer = std::make_unique<FakeWebRenderer>();
		auto compositor = std::make_unique<FakeCompositor>();
		auto* rendererPtr = renderer.get();
		auto* compositorPtr = compositor.get();

		Presentation::WebViewPresenter presenter{
			std::move(renderer),
			std::move(compositor),
			&DrawAvailableForTest,
			&HandleFrameworkKeyForTest
		};

		assert(presenter.Initialize("test-data/OSFUI"));
		presenter.SetDrawPathInstalled(true);
		assert(presenter.Show(MenuView()));

		rendererPtr->frame = OSFUI::FrameBufferView{
			.width = 2560,
			.height = 1440,
			.frameIndex = 42,
			.sharedSlot = 2
		};

		presenter.Tick();

		assert(rendererPtr->updateDeltas.size() == 1);
		assert(rendererPtr->updateDeltas[0] >= 0.0);
		assert(rendererPtr->updateDeltas[0] <= 0.1);
		assert(compositorPtr->submittedFrames.size() == 1);
		assert(compositorPtr->submittedFrames[0].frameIndex == 42);
		assert(compositorPtr->visibleStates.back());

		g_drawAvailable = false;
		presenter.Tick();

		assert(rendererPtr->updateDeltas.size() == 2);
		assert(!compositorPtr->visibleStates.back());
	}

	void TestPresenterConvertsAndCoalescesMouseInput()
	{
		g_drawAvailable = true;

		auto renderer = std::make_unique<FakeWebRenderer>();
		auto compositor = std::make_unique<FakeCompositor>();
		auto* rendererPtr = renderer.get();
		auto* compositorPtr = compositor.get();

		Presentation::WebViewPresenter presenter{
			std::move(renderer),
			std::move(compositor),
			&DrawAvailableForTest,
			&HandleFrameworkKeyForTest
		};

		assert(presenter.Initialize("test-data/OSFUI"));
		presenter.SetDrawPathInstalled(true);
		assert(presenter.Show(MenuView()));
		presenter.SetInputFocus(true);
		compositorPtr->EmitResize(1280, 720);

		presenter.SendKeyEvent(0x41, true);
		assert((rendererPtr->keyEvents ==
			std::vector<std::pair<std::uint32_t, bool>>{
				{ 0x41, true }
			}));

		presenter.UpdateMousePosition(960, 540, 1920, 1080);
		assert(rendererPtr->mouseMoves.empty());
		presenter.Tick();
		assert((rendererPtr->mouseMoves ==
			std::vector<std::pair<int, int>>{
				{ 640, 360 }
			}));

		presenter.UpdateMousePosition(-100, -100, 1920, 1080);
		presenter.Tick();
		assert(rendererPtr->mouseMoves.back() ==
			(std::pair<int, int>{ 0, 0 }));

		presenter.UpdateMousePosition(100, 100, 1920, 1080);
		presenter.UpdateMousePosition(3000, 2000, 1920, 1080);
		presenter.Tick();
		assert(rendererPtr->mouseMoves.size() == 3);
		assert(rendererPtr->mouseMoves.back() ==
			(std::pair<int, int>{ 1279, 719 }));

		presenter.SendMouseButtonEvent(0, true);
		presenter.SendMouseWheelEvent(120);

		assert(rendererPtr->mouseButtons.size() == 1);
		assert(rendererPtr->mouseButtons[0].x == 1279);
		assert(rendererPtr->mouseButtons[0].y == 719);
		assert(rendererPtr->mouseButtons[0].button == 0);
		assert(rendererPtr->mouseButtons[0].down);

		assert(rendererPtr->physicalMouseWheels.size() == 1);
		assert(rendererPtr->physicalMouseWheels[0].x == 1279);
		assert(rendererPtr->physicalMouseWheels[0].y == 719);
		assert(rendererPtr->physicalMouseWheels[0].wheelDelta == 120);

		presenter.SetInputFocus(false);
		presenter.SendKeyEvent(0x42, true);
		presenter.UpdateMousePosition(100, 100, 1920, 1080);
		presenter.SendMouseButtonEvent(0, false);
		presenter.SendMouseWheelEvent(-120);
		presenter.Tick();

		assert(rendererPtr->keyEvents.size() == 1);
		assert(rendererPtr->mouseMoves.size() == 3);
		assert(rendererPtr->mouseButtons.size() == 1);
		assert(rendererPtr->physicalMouseWheels.size() == 1);
	}

	void TestPresenterInitializationFailure()
	{
		auto renderer = std::make_unique<FakeWebRenderer>();
		auto compositor = std::make_unique<FakeCompositor>();
		auto* rendererPtr = renderer.get();
		auto* compositorPtr = compositor.get();

		rendererPtr->initializeSucceeds = false;

		Presentation::WebViewPresenter presenter{
			std::move(renderer),
			std::move(compositor),
			&DrawAvailableForTest,
			&HandleFrameworkKeyForTest
		};

		assert(!presenter.Initialize("test-data/OSFUI"));
		assert(rendererPtr->initializeCalls == 1);
		assert(compositorPtr->initializeCalls == 0);
	}

	void TestPresenterRequiresFrameworkKeyHandler()
	{
		auto renderer = std::make_unique<FakeWebRenderer>();
		auto compositor = std::make_unique<FakeCompositor>();
		auto* rendererPtr = renderer.get();
		auto* compositorPtr = compositor.get();

		Presentation::WebViewPresenter presenter{
			std::move(renderer),
			std::move(compositor),
			&DrawAvailableForTest,
			nullptr
		};

		assert(!presenter.Initialize("test-data/OSFUI"));
		assert(rendererPtr->initializeCalls == 0);
		assert(compositorPtr->initializeCalls == 0);
	}

	void TestPresenterClearsAcceleratorsWhenNativeFocusFails()
	{
		auto renderer = std::make_unique<FakeWebRenderer>();
		auto compositor = std::make_unique<FakeCompositor>();
		auto* rendererPtr = renderer.get();

		Presentation::WebViewPresenter presenter{
			std::move(renderer),
			std::move(compositor),
			&DrawAvailableForTest,
			&HandleFrameworkKeyForTest
		};

		assert(presenter.Initialize("test-data/OSFUI"));
		rendererPtr->throwOnNativeFocus = true;
		presenter.SetInputFocus(true);

		assert((rendererPtr->inputStateTransitions ==
			std::vector<std::string>{ "accelerators:on", "focus:on", "accelerators:off" }));
		assert(rendererPtr->acceleratorStates.size() == 2);
		assert(rendererPtr->acceleratorStates[0].captured);
		assert(!rendererPtr->acceleratorStates[1].captured);
		assert(rendererPtr->nativeFocusStates.empty());
	}

	void TestPresenterContainsNestedTickFailures()
	{
		auto renderer = std::make_unique<FakeWebRenderer>();
		auto compositor = std::make_unique<FakeCompositor>();
		auto* rendererPtr = renderer.get();
		auto* compositorPtr = compositor.get();

		Presentation::WebViewPresenter presenter{
			std::move(renderer),
			std::move(compositor),
			&DrawAvailableForTest,
			&HandleFrameworkKeyForTest
		};

		assert(presenter.Initialize("test-data/OSFUI"));
		rendererPtr->throwOnUpdate = true;
		compositorPtr->throwOnSetVisible = true;

		// Tick is noexcept even when both the original backend operation and
		// the emergency compositor hide fail.
		presenter.Tick();
		assert(rendererPtr->updateDeltas.size() == 1);
	}

	void TestPresenterDestroysRendererBeforeCompositor()
	{
		std::vector<std::string> destructionLog;

		{
			auto renderer = std::make_unique<FakeWebRenderer>(&destructionLog);
			auto compositor = std::make_unique<FakeCompositor>(&destructionLog);

			Presentation::WebViewPresenter presenter{
				std::move(renderer),
				std::move(compositor),
				&DrawAvailableForTest,
				&HandleFrameworkKeyForTest
			};
		}

		assert((destructionLog ==
			std::vector<std::string>{ "renderer", "compositor" }));
	}
}

void RunWebViewPresenterTests()
{
	TestPresenterInitializationAndTransportWiring();
	TestPresenterShowHideAndManifestConversion();
	TestPresenterTickSubmitsFramesAndTracksDrawAvailability();
	TestPresenterConvertsAndCoalescesMouseInput();
	TestPresenterInitializationFailure();
	TestPresenterRequiresFrameworkKeyHandler();
	TestPresenterClearsAcceleratorsWhenNativeFocusFails();
	TestPresenterContainsNestedTickFailures();
	TestPresenterDestroysRendererBeforeCompositor();
}
