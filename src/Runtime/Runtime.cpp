#include "Runtime/Runtime.h"

#include <cmath>
#include <limits>

#include "API/BridgeApi.h"
#include "API/PapyrusApi.h"
#include "Compat/V1/Papyrus.h"
#include "Composite/UiPass.h"
#include "Core/Log.h"
#include "Core/Version.h"
#include "Input/ControlLayer.h"
#include "Input/FocusMenu.h"
#include "Input/FreeCursor.h"
#include "Input/HardwareCursor.h"
#include "Input/KeyNames.h"
#include "Input/MenuMode.h"
#include "Input/MenuEventSink.h"
#include "Input/OverlayInputHook.h"
#include "Input/PauseMenuEntry.h"
#include "Input/SimPause.h"
#include "Input/UiLayoutGuard.h"
#include "Input/XInputPoller.h"
#include "Core/Paths.h"
#include "Platform/WindowsPlatform.h"
#include "Core/Ids.h"
#include "Render/WebView2HostWebRenderer.h"
#include "Views/BuiltinViewIds.h"

namespace OSFUI
{
	namespace
	{
		struct ViewSize
		{
			std::uint32_t width{ 0 };
			std::uint32_t height{ 0 };
		};

		ViewSize ViewSizeForOutput(const OutputSize& a_output)
		{
			constexpr std::uint32_t kMaxViewHeight = 1440;
			const auto height = (std::min)(a_output.height, kMaxViewHeight);
			return {
				.width = static_cast<std::uint32_t>(std::lround(static_cast<double>(a_output.width) * height / a_output.height)),
				.height = height,
			};
		}
	}

	Runtime& Runtime::Get()
	{
		static Runtime* const instance = new Runtime;
		return *instance;
	}

	bool Runtime::InitializePaths()
	{
		return Paths::Initialize();
	}

	void Runtime::InitializeSettingsModule()
	{
		const auto schemaDir = Paths::DataDir() / "settings";
		const auto valuesDir = schemaDir / "values";
		_settings = std::make_unique<SettingsModule>(schemaDir, valuesDir,
			[this](std::string_view a_mod, std::string_view a_key, const nlohmann::json& a_value) {
				OnSettingChanged(a_mod, a_key, a_value);
			},
			// v1 -> v2 values migration: pre-2.x key names were VK-anchored; reanchor to physical key on layout active right now.
			[](const std::string& a_name) -> std::string {
				const auto vk = Legacy::ResolveKeyNameVk(a_name);
				if (vk == 0) {
					return a_name;
				}
				const auto scan = Platform::VkToDirectInputScan(vk);
				if (scan == 0) {
					return a_name;
				}
				auto name = KeyName(static_cast<ScanCode>(scan));
				return name.empty() ? a_name : name;
			});

		const auto* configured = _settings->Store().GetValue("osfui", "developerMode");
		if (configured && configured->is_boolean()) {
			_developerMode = configured->get<bool>();
		} else {
			_developerMode = false;
			REX::WARN("Runtime: setting 'osfui.developerMode' is unavailable or invalid; developer mode disabled");
		}
		Log::SetDebugLogging(_developerMode);
		REX::INFO("Runtime: developer mode = {} (restart-latched from osfui.developerMode)", _developerMode);

		configured = _settings->Store().GetValue("osfui", "highRefreshCapture");
		if (configured && configured->is_boolean()) {
			_highRefreshCapture = configured->get<bool>();
		} else {
			_highRefreshCapture = false;
			REX::WARN("Runtime: setting 'osfui.highRefreshCapture' is unavailable or invalid; high-refresh capture disabled");
		}
		REX::INFO("Runtime: high-refresh capture = {} (restart-latched from osfui.highRefreshCapture)", _highRefreshCapture);
	}

	void Runtime::LoadLocalization()
	{
		_localization.Load(Paths::DataDir() / "l10n", LocalizationService::DetectGameLocale(Paths::StarfieldUserDir()));
	}

	void Runtime::LoadStartupContent()
	{
		_views.DiscoverAll(Paths::ViewsDir());
		_viewPolicy.Load(Paths::DataDir() / "state" / "view-policy.json");

		std::vector<std::string> discoveredViewIds;
		discoveredViewIds.reserve(_views.All().size());

		for (const auto& manifest : _views.All()) {
			discoveredViewIds.push_back(manifest.id);
		}

		API::BridgeApi::Get().SetViewCatalog(discoveredViewIds);
	}

	bool Runtime::InitializeRenderer()
	{
		_renderer = std::make_unique<WebView2HostWebRenderer>();

		const auto* view = _views.Find(Ids::kSettingsViewId);
		const auto initialWidth = view ? view->width : kDefaultViewWidth;
		const auto initialHeight = view ? view->height : kDefaultViewHeight;

		_viewWidth.store(initialWidth);
		_viewHeight.store(initialHeight);
		_cursorX = initialWidth * 0.5f;
		_cursorY = initialHeight * 0.5f;

		WebView2HostConfig rendererConfig{
			.width = initialWidth,
			.height = initialHeight,
			.devMode = _developerMode,
			.highRefreshCapture = _highRefreshCapture,
			.dataDir = Paths::DataDir(),
		};

		if (!_renderer->Initialize(rendererConfig)) {
			REX::ERROR("Runtime: WebView2 renderer failed to initialize");
			return false;
		}

		return true;
	}

	void Runtime::WireRendererLifecycleCallbacks()
	{
		_renderer->SetLoadHandler([this](const WebView2HostWebRenderer::LoadEvent& a_e) {
			OnViewLoad(a_e.viewId, a_e.failed, a_e.url, a_e.description, a_e.errorCode);
		});

		_renderer->SetFailureHandler([this](const WebView2HostWebRenderer::FailureEvent& a_e) {
			OnRendererFailure(a_e);
		});

		_renderer->SetHealthHandler([this](const WebView2HostWebRenderer::HealthEvent& a_e) {
			_runtimeHealth.OnRendererHealth(a_e);
		});

		_renderer->SetCursorChangeHandler([](CursorShape a_shape) {
			HardwareCursor::SetShape(a_shape);
		});
	}

	bool Runtime::InitializeCompositor()
	{
		_compositor = std::make_unique<D3D12Compositor>();
		if (!_compositor->Initialize()) {
			REX::ERROR("Runtime: D3D12 compositor failed to initialize");
			return false;
		}
		return true;
	}

	void Runtime::WireRenderPipeline()
	{
		_renderer->SetSharedRingHandler([this](const SharedRingDesc& a_desc) {
			if (_compositor) {
				_compositor->SetSharedRing(a_desc);
			}
		});
	}

	void Runtime::InitializeFeatureModules()
	{
		_settings->Store().SetTextResolver([this](std::string_view a_mod, std::string_view a_address, std::string_view a_english) {
			return _localization.Resolve(a_mod, a_address, a_english);
		});

		auto& store = _settings->Store();
		store.AddChangeListener([](std::string_view a_mod, std::string_view a_key, const nlohmann::json& a_value) {
			auto& api = API::BridgeApi::Get();
			api.Mirror().Update(a_mod, a_key, a_value);
			api.Subscriptions().OnChanged(a_mod, a_key, a_value);
			API::Papyrus::OnSettingChanged(a_mod, a_key);
		});
		store.AddRegistryListener([this] {
			if (_settings) {  // teardown guard (_settings nulls before modules die)
				API::BridgeApi::Get().Mirror().Rebuild(_settings->Store().DataView());
			}
		});

		store.SetKeyNameResolver(ResolveKeyName);

		_hotkeys.SetSuppression([this] { return IsInputCaptured() || _captureArmed.load(); });
		store.AddChangeListener([this](std::string_view a_mod, std::string_view a_key, const nlohmann::json&) {
			if (_settings && _settings->Store().GetSettingType(a_mod, a_key) == "key") {
				_hotkeys.Rebuild(_settings->Store());
			}
		});
		store.AddRegistryListener([this] {
			if (_settings) {
				_hotkeys.Rebuild(_settings->Store());
			}
		});
		_hotkeys.Rebuild(store);  // LoadAll already ran in the module's constructor

		RefreshKeyboardLabels("startup");

		_settings->OnStart();
	}

	void Runtime::InitializeBridge()
	{
		_bridge = std::make_unique<MessageBridge>([this](std::string_view a_viewId, std::string_view a_json) {
			if (_renderer) {
				_renderer->SendMessageToWeb(a_viewId, a_json);
			}
		});
		
		_bridge->SetHelloHook([this](std::string_view a_viewId) { OnViewGreeted(a_viewId); });

		_bridge->SetProtocolFaultSink([this](std::string_view a_viewId, std::string_view a_code, std::string_view a_message, 
			const nlohmann::json& a_detail, bool a_viewFault) {
			OnProtocolFault(a_viewId, a_code, a_message, a_detail, a_viewFault);
		});

		RegisterPlatformEndpoints(*_bridge);

		_settings->RegisterEndpoints(*_bridge);
		_healthRegistry.AttachBridge(*_bridge);

		_renderer->SetWebMessageHandler([this](std::string_view a_viewId, std::string_view a_json) {
			if (_bridge) {
				_bridge->HandleWebMessage(a_viewId, a_json);
			}
		});
	}

    void Runtime::InitializeStartupViews()
    {
		std::size_t instantiated = 0;
		for (const auto& manifest : _views.All()) {
			if (manifest.kind != ViewKind::Hud) {
				continue;
			}
			if (!HudAutoStartEligible(manifest)) {
				if (_viewPolicy.HasHudOverride(manifest.id)) {
					REX::DEBUG("Runtime: HUD '{}' has an auto-start override but is not eligible (catalog-hidden via hub:false, or debugOnly without developer mode); ignored", manifest.id);
				}
				continue;
			}
			if (!_viewPolicy.HudAutoStart(manifest.id, manifest.openOnStart)) {
				continue;
			}
			if (InstantiateView(manifest, "for HUD auto-start")) {
				++instantiated;
				if (_presentation.IsInstantiated(manifest.id)) {
					_presentation.Open(manifest.id);
				}
			}
		}
		REX::INFO("Runtime: instantiated {} auto-start HUD view(s); default menu = '{}'", instantiated, Ids::kSettingsViewId);
		if (!_views.Find(Ids::kSettingsViewId)) {
			REX::WARN("Runtime: default view '{}' was not discovered; the toggle key will have nothing to open", Ids::kSettingsViewId);
		}
    }

    void Runtime::ConfigureInputRouting()
    {
		const auto toggleKey = ResolveKeyName(Ids::kToggleKey);
		_toggleKey.store(toggleKey, std::memory_order_release);

		if (toggleKey != kInvalidScanCode) {
			REX::INFO("Runtime: toggleKey '{}' resolved to scan code {:#x}", Ids::kToggleKey, toggleKey);
		}

		_renderer->SetNativeAcceleratorHandler([this](std::uint32_t a_vkCode, std::uint32_t a_scanCode, bool a_down) {
			return OnNativeAcceleratorKey(a_vkCode, a_scanCode, a_down);
		});
    }

    bool Runtime::Initialize()
	{
		if (_initialized) {
			return true;
		}
		_rendererFailed = false;
		_rendererFailureLatched = false;
		_browserHostRecovery.Reset();

		if(!InitializePaths()) {
			return false;
		}

		LoadLocalization();
		InitializeSettingsModule();
		InitializeFeatureModules();
		LoadStartupContent();

		if(!InitializeRenderer()) {
			return false;
		}

		WireRendererLifecycleCallbacks();

		if(!InitializeCompositor()) {
			return false;
		}
		WireRenderPipeline();
		InitializeBridge();
		InitializeStartupViews();
		ConfigureInputRouting();

		if (_developerMode) {
			_devViewReload = std::make_unique<DevViewReloadWorker>(Paths::ViewsDir(), [this](std::string_view a_id) {
				return _renderer && _renderer->RefreshViewFiles(a_id);
			});
		}

		_initialized = true;
		// Push the initial policy derived from whatever is open (incl. nothing).
		ApplyViewPresentationPolicy();

		bool pauseMenuEntryEnabled = true;
		if (_settings) {
			if (const auto* value = _settings->Store().GetValue("osfui", "pauseMenuEntry");
				value && value->is_boolean()) {
				pauseMenuEntryEnabled = value->get<bool>();
			}
		}
		if (pauseMenuEntryEnabled) {
			PauseMenuEntry::Install();
		} else {
			REX::INFO("PauseMenuEntry: disabled by startup setting");
		}

		return true;
	}

	bool Runtime::InstallOverlayDrawPath()
	{
		if (!_compositor) {
			return false;
		}
		if (!UiPass::Install()) {
			REX::ERROR("Runtime: Scaleform UI pass hook failed");
			return false;
		}
		return true;
	}

	void Runtime::OnDataLoaded()
	{
		_controlMapInit.Request();
	}

	void Runtime::OnPostDataLoaded()
	{
		_uiIntegrationInit.Request();
	}

	void Runtime::InitializeDataLoadedState()
	{
		REX::DEBUG("Runtime: consuming kPostDataLoad work on the main-thread tick");
		API::Papyrus::Install();
		_controlMap.Initialize();
		SyncLiveControlMapBindings();
		_runtimeHealth.SyncControlMap();
		PublishPlatformState("keybindings");
		PublishPlatformState("input-context");
	}

	void Runtime::InitializePostDataLoadIntegration()
	{
		REX::DEBUG("Runtime: consuming kPostPostDataLoad work on the main-thread tick");
		if (!UiLayoutGuard::VerifyUiLayout()) {
			REX::ERROR("Runtime: UI layout guard failed; skipping ALL UI integration (menu events, FocusMenu and the WndProc hook stay uninstalled; capturing menus are unavailable)");
			return;
		}
		const bool menuEventsInstalled = MenuEventSink::Install();
		const bool focusMenuRegistered = FocusMenu::Register();
		const bool inputInstalled = OverlayInputHook::Install();
		_captureIntegrationAvailable = menuEventsInstalled && focusMenuRegistered && inputInstalled;
		if (!_captureIntegrationAvailable) {
			REX::ERROR("Runtime: required input integration is unavailable; menus that capture input will be refused this session");
			return;
		}
	}

	void Runtime::EnqueuePresentationRequest(ViewPresentationRequest a_req)
	{
		m_viewRequests.Enqueue(a_req);
	}

	void Runtime::EnqueueOpenView(std::string a_viewId)
	{
		m_viewRequests.EnqueueOpen(std::move(a_viewId));
	}


	Runtime::PendingPresentationWork Runtime::TakePresentationRequests(std::vector<API::BridgeApi::ViewPresentationRequest> a_plugin)
	{
		auto queued = m_viewRequests.Take();
		PendingPresentationWork work;
		work.local = std::move(queued.presentation);
		work.openViews = std::move(queued.openViews);
		work.plugin = std::move(a_plugin);
		return work;
	}

	void Runtime::PreparePresentationRequests(const PendingPresentationWork& a_work)
	{
		const auto prepare = [this](std::string_view a_id, std::string_view a_reason) {
			if (_presentation.IsInstantiated(a_id)) {
				return;
			}
			if (const auto* manifest = _views.Find(a_id)) {
				InstantiateView(*manifest, a_reason);
			}
		};

		for (const auto& id : a_work.openViews) {
			prepare(id, "on demand");
		}
		for (const auto& request : a_work.plugin) {
			if (request.open) {
				prepare(request.view, "on demand");
			}
		}
		if (!_pendingViewOpen && !_presentation.ActiveMenu() &&
			std::ranges::find(a_work.local, ViewPresentationRequest::ToggleDefault) != a_work.local.end()) {
			if (!_presentation.IsInstantiated(Ids::kSettingsViewId)) {
				BeginColdOpenTiming(Ids::kSettingsViewId);
			}
			prepare(Ids::kSettingsViewId, "for the default-menu toggle");
		}
	}

	void Runtime::ApplyPresentationRequests(const PendingPresentationWork& a_work)
	{
		const auto& reqs = a_work.local;
		const auto& pluginReqs = a_work.plugin;
		if (reqs.empty() && pluginReqs.empty() && a_work.openViews.empty()) {
			return;
		}
		for (const auto req : reqs) {
			switch (req) {
			case ViewPresentationRequest::ToggleDefault:
				if (_pendingViewOpen) {
					CancelPendingOpen();
				} else if (_presentation.ActiveMenu()) {
					_presentation.CloseActiveMenu();
				} else {
					BeginViewOpen(Ids::kSettingsViewId);
				}
				break;
			case ViewPresentationRequest::Back: {
				const auto active = _presentation.ActiveMenu();
				if (_pendingViewOpen) {
					CancelPendingOpen();
				} else if (active && m_viewInputGrants.OwnsBackAction(*active) && _renderer) {
					constexpr std::uint32_t kVkEscape = 0x1B;
					_renderer->InjectKeyEvent(kVkEscape, true);
					_renderer->InjectKeyEvent(kVkEscape, false);
				} else {
					_presentation.CloseActiveMenu();
				}
				break;
			}
			case ViewPresentationRequest::CloseAll:
				CancelPendingOpen();
				_presentation.CloseAll();
				break;
			}
		}
		for (const auto& id : a_work.openViews) {
			if (!_presentation.IsInstantiated(id)) {
				REX::WARN("Runtime: EnqueueOpenView('{}') ignored — no discovered view could be instantiated", id);
			} else {
				BeginViewOpen(id);
			}
		}
		for (const auto& r : pluginReqs) {
			if (r.open) {
				if (!_presentation.IsInstantiated(r.view)) {
					REX::WARN("Runtime: plugin RequestMenu('{}', open) could not instantiate the discovered view", r.view);
				} else {
					BeginViewOpen(r.view);
				}
			} else {
				if (_pendingViewOpen && *_pendingViewOpen == r.view) {
					CancelPendingOpen();
				}
				_presentation.Close(r.view);
			}
		}
		ApplyViewPresentationPolicy();
	}

	bool Runtime::OverlayCanDraw() const
	{
		return UiPass::DrawEnabled();
	}

	bool Runtime::BeginViewOpen(std::string_view a_id)
	{
		// Require both installation and the lazy render-worker self-test before allowing input capture.
		if (!OverlayCanDraw()) {
			CancelColdOpenTiming(a_id);
			REX::WARN("Runtime: cannot open '{}' — the Scaleform UI draw path is unavailable", a_id);
			return false;
		}
		if (_rendererFailed) {
			CancelColdOpenTiming(a_id);
			if (_browserHostRecovery.RequestManualRetry(_uptime)) {
				REX::INFO("Runtime: open of '{}' requested a fresh browser-host recovery cycle; the overlay remains closed until the replacement reaches its reveal gate", a_id);
			} else if (_browserHostRecovery.PhaseValue() ==
				BrowserHostRecovery::Phase::Waiting ||
				_browserHostRecovery.PhaseValue() ==
				BrowserHostRecovery::Phase::AwaitingResponse) {
				REX::WARN("Runtime: cannot open '{}' yet - the browser host is recovering", a_id);
			} else {
				REX::WARN("Runtime: cannot open '{}' - the web renderer needs a game restart or "
					"the repair described in the log", a_id);
			}
			return false;
		}
		if (!_presentation.IsInstantiated(a_id)) {
			CancelColdOpenTiming(a_id);
			return false;
		}
		const auto* manifest = _views.Find(a_id);
		if (manifest && manifest->kind == ViewKind::Menu && manifest->capturesInput &&
			!_captureIntegrationAvailable) {
			CancelColdOpenTiming(a_id);
			REX::WARN("Runtime: cannot open '{}' — required input integration is unavailable", a_id);
			return false;
		}
		if (!manifest || manifest->kind == ViewKind::Hud) {
			CancelPendingOpen();
			return _presentation.Open(a_id);
		}

		const auto loadState = m_viewLoads.GetState(a_id);
		if (loadState == ViewLoadState::Finished) {
			CancelPendingOpen();
			return _presentation.Open(a_id);
		}
		if (_pendingViewOpen && *_pendingViewOpen == a_id) {
			return false;
		}

		CancelPendingOpen();
		_pendingViewOpen = std::string(a_id);
		REX::DEBUG("Runtime: holding first open of '{}' until its main-frame load succeeds", a_id);
		return true;
	}

	bool Runtime::CancelPendingOpen()
	{
		if (!_pendingViewOpen) {
			return false;
		}
		const auto target = std::move(*_pendingViewOpen);
		_pendingViewOpen.reset();
		CancelColdOpenTiming(target);
		REX::DEBUG("Runtime: cancelled pending open of '{}'", target);
		return true;
	}

	void Runtime::BeginColdOpenTiming(std::string_view a_viewId)
	{
		const auto now = ColdOpenClock::now();
		const auto requestedNanos = _lastToggleRequestNanos.exchange(0, std::memory_order_acq_rel);
		auto requestedAt = now;
		if (requestedNanos > 0) {
			const auto candidate = ColdOpenClock::time_point(std::chrono::nanoseconds(requestedNanos));
			if (candidate <= now) {
				requestedAt = candidate;
			}
		}
		_coldOpenTiming = ColdOpenTiming{
			.viewId = std::string(a_viewId),
			.requestedAt = requestedAt,
		};
	}

	void Runtime::CancelColdOpenTiming(std::string_view a_viewId)
	{
		if (_coldOpenTiming && _coldOpenTiming->viewId == a_viewId) {
			_coldOpenTiming.reset();
		}
	}

	void Runtime::FinishColdOpenTiming(std::string_view a_viewId)
	{
		if (!_coldOpenTiming || _coldOpenTiming->viewId != a_viewId ||
			!_coldOpenTiming->instantiatedAt || !_coldOpenTiming->loadedAt) {
			return;
		}

		const auto timing = std::move(*_coldOpenTiming);
		_coldOpenTiming.reset();
		const auto revealedAt = ColdOpenClock::now();
		const auto milliseconds = [](ColdOpenClock::time_point a_begin, ColdOpenClock::time_point a_end) {
			return std::chrono::duration_cast<std::chrono::milliseconds>(a_end - a_begin).count();
		};
		REX::INFO("Runtime: cold-open timing '{}': {} ms total (input->instantiate {} ms, instantiate->load {} ms, load->presentable-frame {} ms)", 
			timing.viewId, milliseconds(timing.requestedAt, revealedAt), milliseconds(timing.requestedAt, *timing.instantiatedAt), milliseconds(*timing.instantiatedAt, *timing.loadedAt), milliseconds(*timing.loadedAt, revealedAt));
	}

	void Runtime::DrivePendingOpen()
	{
		if (!_pendingViewOpen) {
			return;
		}
		const auto target = *_pendingViewOpen;
		const auto* manifest = _views.Find(target);
		if (!manifest || !_presentation.IsInstantiated(target)) {
			REX::WARN("Runtime: cancelling pending open of '{}' because the view is no longer available", target);
			CancelPendingOpen();
			return;
		}

		if (m_viewLoads.GetState(target) != ViewLoadState::Finished) {
			return;
		}

		_presentation.Open(target);
		_pendingViewOpen.reset();
		REX::DEBUG("Runtime: main-frame load completed; opening '{}'", target);
		ApplyViewPresentationPolicy();
	}

	void Runtime::DrainViewRegistrations(std::vector<std::string> a_ids)
	{
		if (a_ids.empty()) {
			return;
		}
		if (!_renderer) {
			// Drop requests when the overlay has no viable renderer.
			for (const auto& id : a_ids) {
				REX::WARN("Runtime: plugin RegisterView('{}') ignored — overlay not running", id);
			}
			return;
		}
		bool catalogChanged = false;
		for (const auto& id : a_ids) {
			// Do not re-register instantiated views and discard their page state.
			if (_presentation.IsInstantiated(id)) {
				REX::DEBUG("Runtime: plugin RegisterView('{}') — already instantiated, left untouched", id);
				continue;
			}
			const auto* m = _views.Find(id);
			if (!m) {
				REX::WARN("Runtime: plugin RegisterView('{}') ignored — no views/{}/manifest.json was discovered at boot (ids are qualified '<modId>/<view>'; is the view folder installed?)", id, id);
				continue;
			}
			if (m->openOnStart) {
				if (!InstantiateView(*m, "via plugin RegisterView openOnStart")) {
					continue;
				}
				BeginViewOpen(id);
				catalogChanged = true;
			} else {
				// Discovery catalogues the view; registration validates intent without creating the page.
				REX::DEBUG("Runtime: plugin RegisterView('{}') accepted; creation deferred until first open", id);
			}
		}
		if (catalogChanged) {
			ApplyViewPresentationPolicy();     // openOnStart / z-band changes take effect now
			BroadcastViewsData();  // Mod Settings picks the new view up live
		}
	}

	void Runtime::DrainSchemaOps(std::vector<API::BridgeApi::SchemaOp> a_ops)
	{
		if (!_settings || a_ops.empty()) {
			return;
		}
		auto& store = _settings->Store();
		for (auto& op : a_ops) {
			if (!op.schema.is_null()) {
				store.RegisterSchema(std::move(op.schema), SettingsStore::Source::kNative);
			} else if (store.GetSource(op.modId) == SettingsStore::Source::kNative) {
				store.RemoveMod(op.modId);
			} else {
				REX::WARN("Runtime: UnregisterSettingsSchema('{}') ignored — not a native-registered schema", op.modId);
			}
		}
	}

	void Runtime::ApplyViewPresentationPolicy()
	{
		if (!_renderer) {
			return;
		}
		
		if (!OverlayCanDraw() && _presentation.ActiveMenu()) {
			REX::WARN("Runtime: closing a requested menu because the Scaleform UI draw path is unavailable");
			_presentation.CloseActiveMenu();
		}

		if (_presentation.DesiredCapture() && !_captureIntegrationAvailable) {
			REX::WARN("Runtime: closing a requested menu because required input integration is unavailable");
			CancelPendingOpen();
			_presentation.CloseActiveMenu();
		}

		for (const auto& layer : _presentation.DesiredLayers()) {
			_renderer->SetViewHidden(layer.id, layer.hidden);
			_renderer->SetViewOrder(layer.id, layer.z);
		}

		const auto active = _presentation.ActiveMenu();
		if (active) {
			_renderer->SetInputTargetView(*active);
		}
		const bool desiredCapture = _presentation.DesiredCapture();
		const bool captureChanged = _captureInput.exchange(desiredCapture) != desiredCapture;
		if (captureChanged) {
			OverlayInputHook::RequestStateRefresh();
			if (!desiredCapture) {
				CancelArmedKeyCapture();
			}
		}

		const bool visible = _presentation.DesiredVisible();
		const bool wasVisible = m_visible.exchange(visible);
		ReconcileNativeFocus();
		if (_compositor) {
			if (visible && !wasVisible) {
				_latestFrame.reset();
				m_viewReveal.Arm();
			} else {
				if (!visible) {
					_latestFrame.reset();
					m_viewReveal.Cancel();  // closed while a reveal was still pending
				}
				if (!m_viewReveal.Pending()) {
					_compositor->SetVisible(visible);
				}
			}
		}

		if (!visible && wasVisible && _settings) {
			_settings->Store().FlushPersistence();
		}

		if (visible) {
			if (!wasVisible) {
				_cursorX = _viewWidth.load() * 0.5f;
				_cursorY = _viewHeight.load() * 0.5f;
			}
			if (active) {
				QueueMouseMove();  // flushed by Tick's once-per-frame move injection
			}
		}

		if (_bridge) {
			const std::string shown = (visible && active) ? *active : std::string();
			if (shown != _lastShownView) {
				const char* reason = (visible == wasVisible) ? "focus" : "overlay";
				if (!_lastShownView.empty()) {
					_bridge->Emit(_lastShownView, "ui.visibility", nlohmann::json{ { "visible", false }, { "reason", reason } });
				}
				if (!shown.empty()) {
					_bridge->Emit(shown, "ui.visibility", nlohmann::json{ { "visible", true }, { "reason", reason } });
				}
				_lastShownView = shown;
			}
		}
		if (visible != wasVisible) {
			REX::INFO("Runtime: overlay visibility -> {} (capture={})", visible, _captureInput.load());
		}

		BroadcastViewsData();
	}

	void Runtime::ReconcileNativeFocus()
	{
		if (!_renderer) {
			return;
		}
		const auto active = _presentation.ActiveMenu();
		const bool wantsCapture = m_visible.load() && _captureInput.load() && active.has_value();
		// Do not move OS focus away from Starfield until its menu stack has admitted the input-owning sentinel. This keeps engine and native ownership on the same edge.
		const bool focusMenuReady = !wantsCapture || (FocusMenu::IsRegistered() && FocusMenu::IsOpenInEngine());
		const bool want = wantsCapture && focusMenuReady;
		const bool refresh = _nativeFocusRefreshRequested.exchange(false) && want;
		if (want == _nativeFocusGranted && !refresh) {
			return;
		}
		_nativeFocusGranted = want;
		_renderer->SetNativeFocus(want);
	}

	bool Runtime::IsVisible() const
	{
		return m_visible.load();
	}

	void Runtime::DriveBrowserHostRecovery()
	{
		if (_browserHostRecovery.ExpireResponseWait(_uptime)) {
			REX::ERROR("Runtime: replacement browser host produced no load response in {:.0f}s", BrowserHostRecovery::kResponseTimeoutSeconds);
			if (_browserHostRecovery.PhaseValue() ==
				BrowserHostRecovery::Phase::Exhausted) {
				REX::ERROR("Runtime: automatic browser-host recovery exhausted; the next explicit menu open will start a fresh retry cycle");
			}
		}

		if (!_browserHostRecovery.BeginDueAttempt(_uptime)) {
			return;
		}

		const auto attempt = _browserHostRecovery.Attempts();
		REX::INFO("Runtime: restarting browser host (attempt {}/{})", attempt, BrowserHostRecovery::kMaxAttempts);
		if (!_renderer || !_renderer->RestartAfterFailure()) {
			REX::ERROR("Runtime: renderer could not reset its failed browser-host connection");
			_browserHostRecovery.OnAttemptSetupFailed(_uptime);
			if (_browserHostRecovery.PhaseValue() == BrowserHostRecovery::Phase::Exhausted) {
				REX::ERROR("Runtime: automatic browser-host recovery exhausted; the next explicit menu open will start a fresh retry cycle");
			}
			return;
		}

		_rendererFailureLatched = false;
		RehydrateRendererAfterRestart();
	}

	void Runtime::RehydrateRendererAfterRestart()
	{
		if (!_renderer || !_bridge) {
			return;
		}

		m_viewRecovery.ClearAll();
		m_viewInputGrants.ResetAll();
		_pendingMouseMove.store(kNoPendingMouseMove);
		_latestFrame.reset();
		m_viewReveal.Reset();
		_nativeFocusGranted = false;
		std::size_t reloaded = 0;
		for (const auto& manifest : _views.All()) {
			if (!_presentation.IsInstantiated(manifest.id)) {
				continue;
			}
			m_viewLoads.BeginLoad(manifest.id);
			_renderer->CreateOrNavigateView(manifest);
			_bridge->OnViewCreated(manifest.id, IsPre2Target(manifest.targetVersion));
			++reloaded;
		}

		_renderer->Resize(_viewWidth.load(), _viewHeight.load());
		_renderer->SetAcceleratorKeys(_toggleKey.load(std::memory_order_acquire),
			false, _captureArmed.load(), _captureUpScan.load());
		ApplyViewPresentationPolicy();
		BroadcastViewsData();
		REX::INFO("Runtime: replayed {} instantiated view(s) to the replacement browser host; overlay left closed", reloaded);
	}

	void Runtime::OnRendererFailure(const WebView2HostWebRenderer::FailureEvent& a_event)
	{
		if (_rendererFailureLatched) {
			return;
		}
		_rendererFailureLatched = true;
		_rendererFailed = true;
		const bool retryableBrowserHostLoss =
			a_event.stage == "host-connection" && _renderer;
		if (retryableBrowserHostLoss) {
			_browserHostRecovery.OnRetryableFailure(_uptime);
			REX::ERROR("Runtime: browser-host connection failed for view '{}' (0x{:08X}): {} - closing the overlay; bounded browser-host recovery is scheduled", a_event.viewId, a_event.errorCode, a_event.description);
			if (_browserHostRecovery.PhaseValue() ==
				BrowserHostRecovery::Phase::Exhausted) {
				REX::ERROR("Runtime: automatic browser-host recovery exhausted; the next explicit menu open will start a fresh retry cycle");
			}
		} else {
			_browserHostRecovery.Disable();
			REX::ERROR("Runtime: renderer failed at '{}' for view '{}' (0x{:08X}): {} - closing the overlay and disabling it for this session", a_event.stage, a_event.viewId, a_event.errorCode, a_event.description);
		}
		m_viewRecovery.ClearAll();

		CancelPendingOpen();
		_presentation.CloseAll();
		ApplyViewPresentationPolicy();

		ReconcileFocusMenu();
		ReconcileControlLayer();
		ReconcileSimPause();
		FreeCursor::Apply(false);
	}

	void Runtime::OnSettingChanged(std::string_view a_modId, std::string_view a_key, const nlohmann::json& a_value)
	{
		if (a_modId != "osfui") {
			return;
		}

		if (a_key == "language" && a_value.is_string()) {
			const auto requested = a_value.get<std::string>();
			const auto locale = requested == "auto" ? LocalizationService::DetectGameLocale(Paths::StarfieldUserDir()) : LocalizationService::NormalizeLocale(requested);
			if (_localization.SetLocale(locale)) {
				RefreshLocalizedData();
			}
		} else if (a_key == "pauseMenuEntry" && a_value.is_boolean()) {
			REX::INFO("Runtime: pause-menu entry setting changed to {}; takes effect on the next launch", a_value.get<bool>());
		} else if (a_key == "developerMode" && a_value.is_boolean()) {
			const auto desired = a_value.get<bool>();
			if (desired != _developerMode) {
				REX::INFO("Runtime: developer mode setting changed to {}; effective mode remains {} until the next launch", desired, _developerMode);
			}
		} else if (a_key == "highRefreshCapture" && a_value.is_boolean()) {
			const auto desired = a_value.get<bool>();
			if (desired != _highRefreshCapture) {
				REX::INFO("Runtime: high-refresh capture setting changed to {}; effective mode remains {} until the next launch", desired, _highRefreshCapture);
			}
		}
	}

	void Runtime::NotifyKeyboardLayoutChanged()
	{
		_keyboardLayoutChanged.store(true);
	}

	void Runtime::RefreshKeyboardLabels(const char* a_reason)
	{
		if (!_settings) {
			return;
		}
		auto labels = BuildKeyLabels(
			Platform::MakeKeyLabelSource(OverlayInputHook::GameWindowHandle()),
			[this](std::string_view a_address, std::string_view a_english) {
				return _localization.Resolve("osfui", a_address, a_english);
			});
		if (labels == _keyLabels) {
			return;  // same layout + locale: nothing to publish
		}
		_keyLabels = std::move(labels);
		_settings->Store().SetKeyboardLabels(_keyLabels.layout, _keyLabels.labels);
		_settings->BroadcastData();
		REX::DEBUG("Runtime: keyboard labels rebuilt ({}; layout '{}', {} keys)", a_reason, _keyLabels.layout, _keyLabels.labels.size());
	}

	std::string Runtime::KeyLabelFor(std::string_view a_name) const
	{
		for (const auto& [name, label] : _keyLabels.labels) {
			if (name == a_name) {
				return label;
			}
		}
		return std::string(a_name);
	}

	void Runtime::RefreshLocalizedData()
	{
		RefreshKeyboardLabels("locale change");
		if (_settings) {
			_settings->Store().InvalidateLocalizedData();
			if (_controlMap.RefreshLabels(/*localizationChanged*/ true)) {
				SyncLiveControlMapBindings();
				PublishPlatformState("keybindings");
			} else if (_controlMap.Initialized() && !_controlMap.Available()) {
				SyncLiveControlMapBindings();
				_runtimeHealth.SyncControlMap();
				PublishPlatformState("keybindings");
				PublishPlatformState("input-context");
			} else {
				_settings->BroadcastData();
			}
		}
		BroadcastViewsData();
		PublishPlatformState("i18n");
	}

	void Runtime::ApplyGameBindingConflictWarnings(bool a_enabled)
	{
		if (!_settings) return;
		if (_settings->Store().SetGameBindingWarningsEnabled(a_enabled)) {
			_settings->BroadcastData();
		}
		REX::DEBUG("Runtime: game-binding conflict warnings {} (read-only game-binding catalog remains available)", a_enabled ? "enabled" : "disabled");
	}

	void Runtime::SyncLiveControlMapBindings()
	{
		if (!_settings) return;
		std::vector<SettingsStore::GameBinding> bindings;
		if (_controlMap.Available()) {
			const auto owner = _localization.Resolve("osfui", "gameBindings.owner", "Starfield");
			bindings.reserve(_controlMap.ConflictBindings().size());
			for (const auto& binding : _controlMap.ConflictBindings()) {
				bindings.push_back({
					binding.event, owner + " (" + binding.title + ")", binding.code,
					KeyName(static_cast<ScanCode>(binding.code)), binding.engineInputContextName, binding.slot,
					binding.classification, binding.definiteModes, binding.possibleModes,
				});
			}
		}
		auto& store = _settings->Store();
		const bool bindingsChanged = store.SetGameBindings(std::move(bindings));
		const bool warningChanged = store.SetGameBindingWarningsEnabled(true);
		if (bindingsChanged || warningChanged) {
			_settings->BroadcastData();
		}
	}

	void Runtime::OnOutputResized(std::uint32_t a_width, std::uint32_t a_height)
	{
		if (a_width == 0 || a_height == 0 || !_renderer) {
			return;
		}
		const auto view = ViewSizeForOutput({ .width = a_width, .height = a_height });

		if (view.width == _viewWidth.load() && view.height == _viewHeight.load()) {
			return;
		}

		_viewWidth.store(view.width);
		_viewHeight.store(view.height);
		_renderer->Resize(view.width, view.height);
		REX::DEBUG("Runtime: output {}x{} -> view resized to {}x{} (aspect-correct)", a_width, a_height, view.width, view.height);
	}

	void Runtime::SubmitFrameIfVisible()
	{
		if (!_initialized || !IsVisible() || !_renderer || !_compositor) {
			return;
		}

		const auto frame = _renderer->TakeLatestFrame();
		if (frame) {
			_latestFrame = *frame;
		}
		const auto outputSize = _compositor->GetObservedOutputSize();
		std::optional<ViewRevealGate::FrameObservation> observation;
		if (_latestFrame) {
			const auto expected = outputSize ? ViewSizeForOutput(*outputSize) : ViewSize{};
			observation = ViewRevealGate::FrameObservation{
				.generation = _latestFrame->ringGeneration,
				.index = _latestFrame->frameIndex,
				.outputSizeKnown = outputSize.has_value(),
				.matchesExpectedSize = _latestFrame->width == expected.width && _latestFrame->height == expected.height,
			};
		}

		const auto decision = m_viewReveal.Observe(observation, _uptime);
		if (decision.submitFrame && frame) {
			_compositor->Submit(*frame);
		} else {
			_compositor->PrepareSharedRing();
		}
		if (decision.reveal) {
			_compositor->SetVisible(true);  // the cached frame is fresh and output-sized
			if (const auto active = _presentation.ActiveMenu()) {
				FinishColdOpenTiming(*active);
			}
			return;
		}
		if (!decision.timedOut) {
			return;
		}

		const auto active = _presentation.ActiveMenu().value_or("<none>");
		REX::ERROR("Runtime: overlay reveal for '{}' produced no presentable frame in {:.1f}s - closing it and releasing input/pause state", active, decision.heldSeconds);
		_coldOpenTiming.reset();
		CancelPendingOpen();
		_presentation.CloseAll();
		ApplyViewPresentationPolicy();

		ReconcileFocusMenu();
		ReconcileControlLayer();
		ReconcileSimPause();
		FreeCursor::Apply(false);
	}

}
