#include "Runtime/Runtime.h"

#include <limits>

#include "API/BridgeApi.h"
#include "API/PapyrusApi.h"
#include "Composite/UiPass.h"
#include "Core/Log.h"
#include "Core/Version.h"
#include "Input/ControlLayer.h"
#include "Input/FocusMenu.h"
#include "Input/FreeCursor.h"
#include "Input/HardwareCursor.h"
#include "Input/MenuMode.h"
#include "Input/MenuEventSink.h"
#include "Input/OverlayInputHook.h"
#include "Input/SimPause.h"
#include "Input/UiLayoutGuard.h"
#include "Input/XInputPoller.h"
#include "Core/Paths.h"
#include "Core/Ids.h"
#include "Render/WebView2HostWebRenderer.h"

namespace OSFUI
{
	Runtime& Runtime::Get()
	{
		static Runtime* const instance = new Runtime;
		return *instance;
	}

	bool Runtime::InitializePaths()
	{
		return Paths::Initialize();
	}

	void Runtime::LoadStartupContent()
	{
		_views.DiscoverAll(Paths::ViewsDir());

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

		const auto initialWidth = kDefaultViewWidth;
		const auto initialHeight = kDefaultViewHeight;

		_captureSize.store(PackViewSize({ initialWidth, initialHeight }));
		_viewSize.store(PackViewSize({ initialWidth, initialHeight }));
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
		_renderer->SetRelativePointerHandler([this](std::string_view a_viewId,
			std::int32_t a_dx, std::int32_t a_dy, std::int32_t a_wheel) {
			OnBrowserHostRelativePointer(a_viewId, a_dx, a_dy, a_wheel);
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

		_renderer->SetWebMessageHandler([this](std::string_view a_viewId, std::string_view a_json) {
			if (_bridge) {
				_bridge->HandleWebMessage(a_viewId, a_json);
			}
		});
	}

    void Runtime::InitializeStartupViews()
    {
		std::size_t queued = 0;
		for (const auto& manifest : _views.All()) {
			if (manifest.kind != ViewKind::Hud) {
				continue;
			}
			if (!HudAutoStartEligible(manifest)) {
				continue;
			}
			EnqueueOpenView(manifest.id);
			++queued;
		}
		REX::INFO("Runtime: queued {} manifest-selected HUD view(s) for lazy startup", queued);
    }

    void Runtime::ConfigureInputRouting()
    {
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

		_initialized = true;
		REX::INFO("Runtime: add-on loaded; waiting for SFSE kPostLoad before acquiring OSF Settings");
		return true;
	}

	void Runtime::OnPostLoad()
	{
		if (_postLoadAttempted) {
			return;
		}
		_postLoadAttempted = true;
		if (!_osfSettings.Initialize()) {
			REX::ERROR("Runtime: OSF Settings dependency unavailable or ABI-incompatible; OSF UI remains inert");
			return;
		}
		_developerMode = _osfSettings.DeveloperMode();
		_highRefreshCapture = _osfSettings.HighRefreshCapture();
		Log::SetDebugLogging(_developerMode);
		LoadStartupContent();
		InitializeStartupViews();

		REX::INFO("Runtime: lightweight add-on ready; WebView2 will initialize on first view demand");
	}

	bool Runtime::EnsureWebRuntime()
	{
		if (_renderer && _compositor && _bridge) return true;
		if (_webRuntimeInitializing || !_osfSettings.Available()) return false;
		_webRuntimeInitializing = true;
		if (!InitializeRenderer()) {
			_osfSettings.ReportFailure("startup.renderer", "webview.renderer-init", "WebView2 renderer failed to initialize");
			_webRuntimeInitializing = false;
			return false;
		}
		WireRendererLifecycleCallbacks();
		if (!InitializeCompositor()) {
			_osfSettings.ReportFailure("startup.compositor", "webview.compositor-init", "D3D12 compositor failed to initialize");
			_renderer.reset();
			_webRuntimeInitializing = false;
			return false;
		}
		WireRenderPipeline();
		InitializeBridge();
		ConfigureInputRouting();
		if (_drawPathRequested && !UiPass::Install()) {
			_osfSettings.ReportFailure("startup.draw-path", "webview.draw-path", "Scaleform UI pass hook failed");
			_webRuntimeInitializing = false;
			return false;
		}
		if (_developerMode) {
			_devViewReload = std::make_unique<DevViewReloadWorker>(Paths::ViewsDir(), [this](std::string_view a_id) {
				return _renderer && _renderer->RefreshViewFiles(a_id);
			});
		}
		_osfSettings.ClearFailure("startup.renderer");
		_osfSettings.ClearFailure("startup.compositor");
		_osfSettings.ClearFailure("startup.draw-path");
		_webRuntimeInitializing = false;
		REX::INFO("Runtime: lazy WebView2 runtime initialized");
		return true;
	}

	bool Runtime::InstallOverlayDrawPath()
	{
		_drawPathRequested = true;
		if (_compositor && !UiPass::Install()) {
			REX::ERROR("Runtime: Scaleform UI pass hook failed");
			return false;
		}
		return true;
	}

	void Runtime::OnDataLoaded()
	{
		_dataLoadedInit.Request();
	}

	void Runtime::OnPostDataLoaded()
	{
		_postDataLoadedReady = true;
	}

	void Runtime::InitializeDataLoadedState()
	{
		REX::DEBUG("Runtime: consuming kPostDataLoad work on the main-thread tick");
		API::Papyrus::Install();
	}

	bool Runtime::EnsureCaptureIntegration()
	{
		if (_captureIntegrationInitialized) return _captureIntegrationAvailable;
		if (!_postDataLoadedReady) return false;
		_captureIntegrationInitialized = true;
		if (!UiLayoutGuard::VerifyUiLayout()) {
			REX::ERROR("Runtime: UI layout guard failed; skipping ALL UI integration (menu events, FocusMenu and the WndProc hook stay uninstalled; capturing menus are unavailable)");
			return false;
		}
		const bool menuEventsInstalled = MenuEventSink::Install();
		const bool focusMenuRegistered = FocusMenu::Register();
		const bool inputInstalled = OverlayInputHook::Install();
		_captureIntegrationAvailable = menuEventsInstalled && focusMenuRegistered && inputInstalled;
		if (!_captureIntegrationAvailable) {
			REX::ERROR("Runtime: required input integration is unavailable; menus that capture input will be refused this session");
			return false;
		}
		REX::INFO("Runtime: lazy web-input hook installed above OSF Settings input handling");
		return true;
	}

	void Runtime::EnqueuePresentationRequest(ViewPresentationRequest a_req)
	{
		m_viewRequests.Enqueue(a_req);
	}

	void Runtime::EnqueueOpenView(std::string a_viewId)
	{
		m_viewRequests.EnqueueOpen(std::move(a_viewId));
	}

	void Runtime::EnqueueRelativePointerCapture(std::string a_viewId, bool a_active)
	{
		m_viewRequests.EnqueueRelativePointer(std::move(a_viewId), a_active);
	}


	Runtime::PendingPresentationWork Runtime::TakePresentationRequests(std::vector<API::BridgeApi::ViewPresentationRequest> a_plugin)
	{
		auto queued = m_viewRequests.Take();
		PendingPresentationWork work;
		work.local = std::move(queued.presentation);
		work.openViews = std::move(queued.openViews);
		work.relativePointer = std::move(queued.relativePointer);
		work.plugin = std::move(a_plugin);
		return work;
	}

	void Runtime::ApplyPresentationRequests(const PendingPresentationWork& a_work)
	{
		const auto& reqs = a_work.local;
		const auto& pluginReqs = a_work.plugin;
		if (reqs.empty() && pluginReqs.empty() && a_work.openViews.empty() && a_work.relativePointer.empty()) {
			return;
		}
		for (const auto req : reqs) {
			switch (req) {
			case ViewPresentationRequest::ToggleDefault:
				// OSF UI 2 has no global toggle or default menu.
				break;
			case ViewPresentationRequest::Back: {
				const auto active = _presentation.ActiveMenu();
				if (_pendingViewOpen) {
					CancelPendingOpen();
				} else if (active) {
					if (const auto target = m_viewInputGrants.BackTargetFor(*active)) {
						BeginViewOpen(*target, "for native back navigation");
					} else if (m_viewInputGrants.OwnsBackAction(*active) && _renderer) {
						constexpr std::uint32_t kVkEscape = 0x1B;
						_renderer->InjectKeyEvent(kVkEscape, true);
						_renderer->InjectKeyEvent(kVkEscape, false);
					} else {
						_presentation.CloseActiveMenu();
					}
				} else {
					_presentation.CloseActiveMenu();
				}
				break;
			}
			case ViewPresentationRequest::CloseAll:
				CancelPendingOpen();
				_viewOpenPreflightBarriers.clear();
				_presentation.CloseAll();
				break;
			}
		}
		for (const auto& request : a_work.openViews) {
			BeginViewOpen(request.view, "on demand", request.requestedAt);
		}
		for (const auto& r : pluginReqs) {
			if (r.open) {
				BeginViewOpen(r.view, "on demand", r.requestedAt);
			} else {
				CancelPendingOpen(r.view);
				_presentation.Close(r.view);
			}
		}
		ApplyViewPresentationPolicy();
		ApplyRelativePointerRequests(a_work.relativePointer);
	}

	void Runtime::ApplyRelativePointerRequests(const std::vector<ViewRequestQueue::RelativePointerRequest>& a_requests)
	{
		for (const auto& request : a_requests) {
			if (!request.active) {
				EndRelativePointerCapture(request.view);
				continue;
			}

			const auto active = _presentation.ActiveMenu();
			if (!IsInputCaptured() || !active || *active != request.view || !_presentation.IsOpen(request.view)) {
				if (_bridge) {
					_bridge->ReportProtocolFault(request.view, "pointer-capture-forbidden",
						"only the visible input-owning menu can capture relative pointer input");
				}
				continue;
			}
			if (!BeginRelativePointerCapture(request.view) && _bridge) {
				_bridge->ReportProtocolFault(request.view, "pointer-capture-unavailable",
					"the native owner did not register a relative pointer handler", {}, false);
			}
		}
	}

	bool Runtime::OverlayCanDraw() const
	{
		return UiPass::DrawEnabled();
	}

	bool Runtime::BeginViewOpen(std::string_view a_id, std::string_view a_reason,
		std::optional<ViewTimingClock::time_point> a_requestedAt)
	{
		if (_presentation.IsOpen(a_id) ||
			(_pendingViewOpen && *_pendingViewOpen == a_id) ||
			_viewOpenPreflightBarriers.contains(std::string(a_id))) {
			return false;
		}
		const auto* manifest = _views.Find(a_id);
		if (!manifest) {
			CancelColdOpenTiming(a_id);
			REX::WARN("Runtime: cannot open '{}' — no discovered view has that id", a_id);
			_osfSettings.ReportFailure("view." + std::string(a_id), "view.not-found",
				"The requested OSF UI view is not installed", { { "view", a_id } });
			return false;
		}
		if (!EnsureWebRuntime()) {
			CancelColdOpenTiming(a_id);
			REX::WARN("Runtime: cannot open '{}' — lazy WebView runtime initialization failed", a_id);
			return false;
		}
		// Require both installation and the lazy render-worker self-test before allowing input capture.
		if (!OverlayCanDraw()) {
			CancelColdOpenTiming(a_id);
			REX::WARN("Runtime: cannot open '{}' — the Scaleform UI draw path is unavailable", a_id);
			_osfSettings.ReportFailure("view." + std::string(a_id), "view.draw-path-unavailable",
				"The view cannot open because the UI draw path is unavailable", { { "view", a_id } });
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
		const bool requiresCaptureIntegration = manifest->kind == ViewKind::Menu && manifest->capturesInput;
		if (requiresCaptureIntegration && !_captureIntegrationInitialized) {
			EnsureCaptureIntegration();
		}
		if (requiresCaptureIntegration && _captureIntegrationInitialized &&
			!_captureIntegrationAvailable) {
			CancelColdOpenTiming(a_id);
			REX::WARN("Runtime: cannot open '{}' — required input integration is unavailable", a_id);
			_osfSettings.ReportFailure("view." + std::string(a_id), "view.input-unavailable",
				"The view requires web input, but input integration is unavailable", { { "view", a_id } });
			return false;
		}

		const auto preflight = API::BridgeApi::Get().RunViewOpenPreflight(a_id);
		if (preflight == API::BridgeApi::ViewOpenPreflightResult::kDenied) {
			CancelColdOpenTiming(a_id);
			REX::WARN("Runtime: native view-open preflight denied '{}'; current presentation retained", a_id);
			return false;
		}
		const bool requiresStateBarrier = preflight == API::BridgeApi::ViewOpenPreflightResult::kAllowed;
		if (requiresStateBarrier) {
			REX::DEBUG("Runtime: native view-open preflight allowed '{}'; holding presentation through the next main tick", a_id);
		}

		if (manifest->kind == ViewKind::Menu &&
			m_viewLoads.GetState(a_id) != ViewLoadState::Finished) {
			BeginColdOpenTiming(a_id, a_requestedAt);
		}
		if (!_presentation.IsInstantiated(a_id) && !InstantiateView(*manifest, a_reason)) {
			CancelColdOpenTiming(a_id);
			return false;
		}

		if (requiresStateBarrier) {
			_viewOpenPreflightBarriers.emplace(std::string(a_id), _mainTickSerial + 1);
		}
		if (manifest->kind == ViewKind::Hud) {
			return requiresStateBarrier || _presentation.Open(a_id);
		}

		const bool waitingForCaptureIntegration = requiresCaptureIntegration && !_captureIntegrationAvailable;
		const auto loadState = m_viewLoads.GetState(a_id);
		if (!requiresStateBarrier && !waitingForCaptureIntegration && loadState == ViewLoadState::Finished) {
			CancelPendingOpen();
			return _presentation.Open(a_id);
		}

		CancelPendingOpen();
		if (!_coldOpenTiming || _coldOpenTiming->viewId != a_id) {
			BeginColdOpenTiming(a_id, a_requestedAt.value_or(ViewTimingClock::now()));
		}
		_pendingViewOpen = std::string(a_id);
		if (requiresStateBarrier) {
			REX::DEBUG("Runtime: holding open of '{}' for its native retained-state barrier", a_id);
		} else if (waitingForCaptureIntegration) {
			REX::DEBUG("Runtime: holding open of '{}' until required input integration initializes", a_id);
		} else {
			REX::DEBUG("Runtime: holding first open of '{}' until its main-frame load succeeds", a_id);
		}
		return true;
	}

	bool Runtime::CancelPendingOpen()
	{
		if (!_pendingViewOpen) {
			return false;
		}
		const auto target = std::move(*_pendingViewOpen);
		_pendingViewOpen.reset();
		_viewOpenPreflightBarriers.erase(target);
		CancelColdOpenTiming(target);
		REX::DEBUG("Runtime: cancelled pending open of '{}'", target);
		return true;
	}

	bool Runtime::CancelPendingOpen(std::string_view a_id)
	{
		if (_pendingViewOpen && *_pendingViewOpen == a_id) {
			return CancelPendingOpen();
		}
		if (_viewOpenPreflightBarriers.erase(std::string(a_id)) > 0) {
			REX::DEBUG("Runtime: cancelled pending open of '{}'", a_id);
			return true;
		}
		return false;
	}

	void Runtime::BeginColdOpenTiming(std::string_view a_viewId,
		std::optional<ViewTimingClock::time_point> a_requestedAt)
	{
		if (_coldOpenTiming && _coldOpenTiming->viewId == a_viewId) {
			return;  // Preserve the earliest request while the same cold open is pending.
		}
		const auto now = ViewTimingClock::now();
		auto requestedAt = now;
		if (a_requestedAt) {
			if (*a_requestedAt != ViewTimingClock::time_point{} && *a_requestedAt <= now) {
				requestedAt = *a_requestedAt;
			}
		}
		_coldOpenTiming = ColdOpenTiming{
			.viewId = std::string(a_viewId),
			.requestedAt = requestedAt,
		};
		if (_presentation.IsInstantiated(a_viewId)) {
			_coldOpenTiming->instantiatedAt = now;
		}
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
		const auto revealedAt = ViewTimingClock::now();
		const auto milliseconds = [](ViewTimingClock::time_point a_begin, ViewTimingClock::time_point a_end) {
			return std::chrono::duration_cast<std::chrono::milliseconds>(a_end - a_begin).count();
		};
		REX::INFO("Runtime: cold-open timing '{}': {} ms total (request->instantiate {} ms, instantiate->load {} ms, load->presentable-frame {} ms)",
			timing.viewId, milliseconds(timing.requestedAt, revealedAt), milliseconds(timing.requestedAt, *timing.instantiatedAt), milliseconds(*timing.instantiatedAt, *timing.loadedAt), milliseconds(*timing.loadedAt, revealedAt));
	}

	void Runtime::BeginHiddenPrewarmTiming(std::string_view a_viewId)
	{
		_hiddenPrewarmTiming = HiddenPrewarmTiming{
			.viewId = std::string(a_viewId),
			.requestedAt = ViewTimingClock::now(),
		};
	}

	void Runtime::CancelHiddenPrewarmTiming(std::string_view a_viewId)
	{
		if (_hiddenPrewarmTiming && _hiddenPrewarmTiming->viewId == a_viewId) {
			_hiddenPrewarmTiming.reset();
		}
	}

	void Runtime::FinishHiddenPrewarmTiming(std::string_view a_viewId, ViewTimingClock::time_point a_loadedAt)
	{
		if (!_hiddenPrewarmTiming || _hiddenPrewarmTiming->viewId != a_viewId ||
			!_hiddenPrewarmTiming->instantiatedAt) {
			return;
		}
		// If the player opened the view before prewarm completed, the cold-open summary owns the useful timing line.
		if (_coldOpenTiming && _coldOpenTiming->viewId == a_viewId) {
			_hiddenPrewarmTiming.reset();
			return;
		}

		const auto timing = std::move(*_hiddenPrewarmTiming);
		_hiddenPrewarmTiming.reset();
		const auto milliseconds = [](ViewTimingClock::time_point a_begin, ViewTimingClock::time_point a_end) {
			return std::chrono::duration_cast<std::chrono::milliseconds>(a_end - a_begin).count();
		};
		REX::INFO("Runtime: hidden-prewarm timing '{}': {} ms total (request->instantiate {} ms, instantiate->load {} ms)", timing.viewId, milliseconds(timing.requestedAt, a_loadedAt), milliseconds(timing.requestedAt, *timing.instantiatedAt), milliseconds(*timing.instantiatedAt, a_loadedAt));
	}

	void Runtime::DrivePendingOpen()
	{
		bool policyChanged = false;
		for (auto it = _viewOpenPreflightBarriers.begin(); it != _viewOpenPreflightBarriers.end();) {
			if (it->second > _mainTickSerial) {
				++it;
				continue;
			}
			const auto* manifest = _views.Find(it->first);
			if (!manifest || !_presentation.IsInstantiated(it->first)) {
				REX::WARN("Runtime: cancelling deferred open of '{}' because the view is no longer available", it->first);
				it = _viewOpenPreflightBarriers.erase(it);
				continue;
			}
			if (manifest->kind == ViewKind::Menu) {
				++it;
				continue;
			}
			policyChanged = _presentation.Open(it->first) || policyChanged;
			REX::DEBUG("Runtime: retained-state barrier completed; opening HUD '{}'", it->first);
			it = _viewOpenPreflightBarriers.erase(it);
		}
		if (policyChanged) {
			ApplyViewPresentationPolicy();
		}
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
		const bool requiresCaptureIntegration = manifest->kind == ViewKind::Menu && manifest->capturesInput;
		if (requiresCaptureIntegration && !_captureIntegrationInitialized) {
			EnsureCaptureIntegration();
		}
		if (requiresCaptureIntegration && !_captureIntegrationInitialized) {
			return;
		}
		if (requiresCaptureIntegration && !_captureIntegrationAvailable) {
			REX::WARN("Runtime: cancelling pending open of '{}' because required input integration failed to initialize", target);
			CancelPendingOpen();
			return;
		}
		if (const auto barrier = _viewOpenPreflightBarriers.find(target);
			barrier != _viewOpenPreflightBarriers.end() && barrier->second > _mainTickSerial) {
			return;
		}

		if (m_viewLoads.GetState(target) != ViewLoadState::Finished) {
			return;
		}

		_presentation.Open(target);
		_pendingViewOpen.reset();
		const bool completedStateBarrier = _viewOpenPreflightBarriers.erase(target) > 0;
		REX::DEBUG("Runtime: {} completed; opening '{}'",
			completedStateBarrier ? "retained-state barrier and main-frame load" : "main-frame load", target);
		ApplyViewPresentationPolicy();
	}

	void Runtime::DrainViewRegistrations(std::vector<std::string> a_ids)
	{
		if (a_ids.empty()) {
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
				catalogChanged = BeginViewOpen(id, "via plugin RegisterView openOnStart") || catalogChanged;
			} else {
				// Discovery catalogues the view; registration validates intent without creating the page.
				REX::DEBUG("Runtime: plugin RegisterView('{}') accepted; creation deferred until first open", id);
			}
		}
		if (catalogChanged) {
			ApplyViewPresentationPolicy();     // openOnStart / z-band changes take effect now
			BroadcastViewsData();
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

		const auto layers = _presentation.DesiredLayers();
		for (const auto& layer : layers) {
			_renderer->SetViewOrder(layer.id, layer.z);
		}
		const auto active = _presentation.ActiveMenu();
		// Publish the input target before its view is shown. The browser host then
		// grants focus only after that target becomes visible.
		if (active) {
			_renderer->SetInputTargetView(*active);
		}
		// A menu switch is intentionally show-before-hide. The browser host keeps the outgoing visual until the incoming view passes its paint handshake;
		for (const auto& layer : layers) {
			if (!layer.hidden) {
				_renderer->SetViewHidden(layer.id, false);
			}
		}
		for (const auto& layer : layers) {
			if (layer.hidden) {
				_renderer->SetViewHidden(layer.id, true);
			}
		}

		const bool desiredCapture = _presentation.DesiredCapture();
		if (!_relativePointerView.empty() && (!desiredCapture || !active || *active != _relativePointerView)) {
			CancelRelativePointerCapture();
		}
		const bool captureChanged = _captureInput.exchange(desiredCapture) != desiredCapture;
		if (captureChanged) OverlayInputHook::RequestStateRefresh();

		const bool visible = _presentation.DesiredVisible();
		const bool wasVisible = m_visible.exchange(visible);
		if (visible && !wasVisible) {
			_renderer->SetPointerInputEnabled(false);
		}
		ReconcileNativeFocus();
		if (!visible) {
			_renderer->SetPointerInputEnabled(true);
		}
		if (_compositor) {
			if (visible && !wasVisible) {
				_latestFrame.reset();
				m_viewReveal.Arm();
				_viewGeometryReady.store(false, std::memory_order_release);
			} else {
				if (!visible) {
					_latestFrame.reset();
					m_viewReveal.Cancel();  // closed while a reveal was still pending
					_viewGeometryReady.store(true, std::memory_order_release);
				}
				if (!m_viewReveal.Pending()) {
					_compositor->SetVisible(visible);
				}
			}
		}

		if (visible) {
			if (!wasVisible) {
				const auto view = UnpackViewSize(_viewSize.load(std::memory_order_acquire));
				_cursorX = view.width * 0.5f;
				_cursorY = view.height * 0.5f;
				_cursorInsideView.store(true, std::memory_order_release);
			}
			if (active && _viewGeometryReady.load(std::memory_order_acquire)) {
				QueueMouseMove();  // flushed by Tick's once-per-frame move injection
			}
		}

		const std::string shown = (visible && active) ? *active : std::string();
		if (shown != _lastShownView) {
			const std::string previous = _lastShownView;
			_lastShownView = shown;
			const char* reason = (visible == wasVisible) ? "focus" : "overlay";
			if (!previous.empty()) {
				API::BridgeApi::Get().DispatchViewLifecycle(previous, API::Views::ViewLifecyclePhase::kHidden);
				if (_bridge) {
					_bridge->Emit(previous, "ui.visibility", nlohmann::json{ { "visible", false }, { "reason", reason } });
				}
			}
			if (!shown.empty()) {
				API::BridgeApi::Get().DispatchViewLifecycle(shown, API::Views::ViewLifecyclePhase::kShown);
				if (_bridge) {
					_bridge->Emit(shown, "ui.visibility", nlohmann::json{ { "visible", true }, { "reason", reason } });
				}
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

		CancelRelativePointerCapture();
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
			_bridge->OnViewCreated(manifest.id);
			++reloaded;
		}

		const auto capture = UnpackViewSize(_captureSize.load(std::memory_order_acquire));
		const auto view = UnpackViewSize(_viewSize.load(std::memory_order_acquire));
		_renderer->Resize(capture.width, capture.height);
		_renderer->SetViewport(view.width, view.height);
		_renderer->SetAcceleratorKeys(kInvalidScanCode, false, false, kInvalidScanCode);
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
		_hiddenPrewarmTiming.reset();

		CancelPendingOpen();
		_viewOpenPreflightBarriers.clear();
		_presentation.CloseAll();
		ApplyViewPresentationPolicy();

		ReconcileFocusMenu();
		ReconcileControlLayer();
		ReconcileSimPause();
		FreeCursor::Apply(false);
	}

	void Runtime::NotifyKeyboardLayoutChanged()
	{
		// OSF Settings owns keyboard labels and layout changes.
	}

	void Runtime::OnOutputResized(std::uint32_t a_width, std::uint32_t a_height)
	{
		if (a_width == 0 || a_height == 0 || !_renderer) {
			return;
		}
		const bool fixedScaleformGeometry =
			MenuEventSink::ChargenOpen() && UiPass::UsesScaleformEnd();
		const auto view = ViewSizeForOutput(
			{ .width = a_width, .height = a_height }, fixedScaleformGeometry);
		UiPass::SetExpectedOutputSize(a_width, a_height);

		const ViewSize output{ .width = a_width, .height = a_height };
		const auto previousCapture = UnpackViewSize(
			_captureSize.load(std::memory_order_acquire));
		const auto previousView = UnpackViewSize(
			_viewSize.load(std::memory_order_acquire));
		const auto observedTarget = _compositor ?
			_compositor->GetObservedOutputSize() : std::nullopt;
		const auto observedWidth = observedTarget ? observedTarget->width : 0;
		const auto observedHeight = observedTarget ? observedTarget->height : 0;
		const bool captureChanged =
			output.width != previousCapture.width ||
			output.height != previousCapture.height;
		const bool viewportChanged =
			view.width != previousView.width || view.height != previousView.height;
		const bool modeChanged = fixedScaleformGeometry != _fixedScaleformGeometry;
		_fixedScaleformGeometry = fixedScaleformGeometry;
		if (!captureChanged && !viewportChanged) {
			if (modeChanged) {
				REX::INFO("Runtime: Scaleform geometry mode -> {} (ChargenMenu={}, handoff={}, client/view {}x{}, last target {}x{})",
					fixedScaleformGeometry ? "fixed-16:9" : "full-output",
					MenuEventSink::ChargenOpen(),
					UiPass::UsesScaleformEnd() ? "ScaleformEnd" : "post-composite",
					a_width, a_height, observedWidth, observedHeight);
			}
			return;
		}

		const bool visible = IsVisible();
		if (visible && _compositor) {
			_compositor->SetVisible(false);
			_renderer->SetPointerInputEnabled(false);
			_latestFrame.reset();
			if (captureChanged) {
				m_viewReveal.ArmForResize();
			} else {
				m_viewReveal.Arm();
			}
			_viewGeometryReady.store(false, std::memory_order_release);
		}
		CancelRelativePointerCapture();
		_pendingMouseMove.store(kNoPendingMouseMove, std::memory_order_release);
		_cursorInsideView.store(false, std::memory_order_release);
		_captureSize.store(PackViewSize(output), std::memory_order_release);
		_viewSize.store(PackViewSize(view), std::memory_order_release);
		if (captureChanged) {
			_renderer->Resize(output.width, output.height);
		}
		if (viewportChanged) {
			_renderer->SetViewport(view.width, view.height);
		}
		REX::INFO("Runtime: Scaleform geometry mode -> {} (ChargenMenu={}, handoff={}, capture {}x{}, viewport {}x{}, last target {}x{})",
			fixedScaleformGeometry ? "fixed-16:9" : "full-output",
			MenuEventSink::ChargenOpen(),
			UiPass::UsesScaleformEnd() ? "ScaleformEnd" : "post-composite",
			a_width, a_height, view.width, view.height, observedWidth, observedHeight);
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
		const auto expected = UnpackViewSize(_captureSize.load(std::memory_order_acquire));
		const bool outputSizeKnown = _gameClientSizeObserved.load(std::memory_order_acquire);
		std::optional<ViewRevealGate::FrameObservation> observation;
		if (_latestFrame) {
			observation = ViewRevealGate::FrameObservation{
				.generation = _latestFrame->ringGeneration,
				.index = _latestFrame->frameIndex,
				.outputSizeKnown = outputSizeKnown,
				.matchesExpectedSize = _latestFrame->width == expected.width && _latestFrame->height == expected.height,
			};
		}

		const auto decision = m_viewReveal.Observe(observation, _uptime);
		if (decision.submitFrame && frame) {
			_compositor->Submit(*frame);
			if (observation && observation->outputSizeKnown && observation->matchesExpectedSize) {
				if (const auto active = _presentation.ActiveMenu()) {
					FinishColdOpenTiming(*active);
				}
			}
		} else {
			_compositor->PrepareSharedRing();
		}
		if (decision.reveal) {
			_compositor->SetVisible(true);  // the cached frame is fresh and output-sized
			_viewGeometryReady.store(true, std::memory_order_release);
			_renderer->SetPointerInputEnabled(true);
			QueueMouseMove();
			return;
		}
		if (!decision.timedOut) {
			return;
		}

		const auto active = _presentation.ActiveMenu().value_or("<none>");
		REX::ERROR("Runtime: overlay reveal for '{}' produced no presentable frame in {:.1f}s - closing it and releasing input/pause state", active, decision.heldSeconds);
		_coldOpenTiming.reset();
		CancelPendingOpen();
		_viewOpenPreflightBarriers.clear();
		_presentation.CloseAll();
		ApplyViewPresentationPolicy();

		ReconcileFocusMenu();
		ReconcileControlLayer();
		ReconcileSimPause();
		FreeCursor::Apply(false);
	}

}
