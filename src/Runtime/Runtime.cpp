#include "Runtime/Runtime.h"

#include <limits>

#include "API/BridgeApi.h"
#include "API/PapyrusApi.h"
#include "Composite/UiPass.h"
#include "Core/Log.h"
#include "Core/Version.h"
#include "Input/FreeCursor.h"
#include "Input/HardwareCursor.h"
#include "Input/MenuEventSink.h"
#include "Input/SimPause.h"
#include "Core/Paths.h"
#include "Core/Ids.h"
#include "Render/WebView2HostWebRenderer.h"
#include "Render/SharedFrameConsumer.h"

namespace OSFUI
{
	Runtime& Runtime::Get()
	{
		static Runtime* const instance = new Runtime;
		return *instance;
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
		auto renderer = std::make_unique<WebView2HostWebRenderer>();

		const auto initialWidth = kDefaultViewWidth;
		const auto initialHeight = kDefaultViewHeight;

		_pointerInput.Initialize({ initialWidth, initialHeight });

		WebView2HostConfig rendererConfig{
			.width = initialWidth,
			.height = initialHeight,
			.devMode = _developerMode,
			.highRefreshCapture = _highRefreshCapture,
			.language = _osfSettings.Language(),
			.dataDir = Paths::DataDir(),
		};

		if (!renderer->Initialize(rendererConfig)) {
			REX::ERROR("Runtime: WebView2 renderer failed to initialize");
			return false;
		}

		_renderer = std::move(renderer);
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

		_renderer->SetCursorChangeHandler([](CursorShape a_shape) {
			HardwareCursor::SetShape(a_shape);
		});
	}

	bool Runtime::InitializeCompositor()
	{
		auto compositor = std::make_unique<D3D12Compositor>();
		if (!compositor->Initialize(_renderer->Frames())) {
			REX::ERROR("Runtime: D3D12 compositor failed to initialize");
			return false;
		}
		_compositor = std::move(compositor);
		return true;
	}

	void Runtime::InitializeBridge()
	{
		if (_bridge) return;
		auto bridge = std::make_unique<MessageBridge>([this](std::string_view a_viewId, std::string_view a_json) {
			if (_renderer) {
				_renderer->SendMessageToWeb(a_viewId, a_json);
			}
		});
		
		bridge->SetHelloHook([this](std::string_view a_viewId) { OnViewGreeted(a_viewId); });

		bridge->SetProtocolFaultSink([this](std::string_view a_viewId, std::string_view a_code, std::string_view a_message,
			const nlohmann::json& a_detail, bool a_viewFault) {
			OnProtocolFault(a_viewId, a_code, a_message, a_detail, a_viewFault);
		});

		RegisterPlatformEndpoints(*bridge);
		_bridge = std::move(bridge);
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

    bool Runtime::Initialize()
	{
		if (_initialized) {
			return true;
		}
		_rendererFailed = false;
		_rendererFailureLatched = false;
		_browserHostRecovery.Reset();

		if (!Paths::Initialize()) {
			return false;
		}

		_initialized = true;
		REX::INFO("Runtime: add-on loaded; waiting for SFSE kPostPostLoad before acquiring OSF Settings");
		return true;
	}

	void Runtime::OnPostPostLoad()
	{
		if (_postPostLoadAttempted) {
			return;
		}
		_postPostLoadAttempted = true;
		if (!_osfSettings.Initialize()) {
			REX::ERROR("Runtime: OSF Settings dependency unavailable or ABI-incompatible; OSF UI remains inert");
			return;
		}
		_developerMode = _osfSettings.DeveloperMode();
		_highRefreshCapture = _osfSettings.HighRefreshCapture();
		Log::SetDebugLogging(_developerMode);
		LoadStartupContent();
		_osfSettings.RegisterLaunchers(_views.All());
		InitializeStartupViews();

		REX::INFO("Runtime: lightweight add-on ready; WebView2 will initialize on first view demand");
	}

	bool Runtime::EnsureWebRuntime()
	{
		if (_webRuntimeReady) return true;
		if (_webRuntimeInitializing || !_osfSettings.Available()) return false;
		_webRuntimeInitializing = true;
		struct ResetInitializing final {
			bool& flag;
			~ResetInitializing() { flag = false; }
		} resetInitializing{ _webRuntimeInitializing };
		if (!_renderer && !InitializeRenderer()) {
			_osfSettings.ReportFailure("startup.renderer", "webview.renderer-init", "WebView2 renderer failed to initialize");
			return false;
		}
		WireRendererLifecycleCallbacks();
		if (!_compositor && !InitializeCompositor()) {
			_osfSettings.ReportFailure("startup.compositor", "webview.compositor-init", "D3D12 compositor failed to initialize");
			return false;
		}
		InitializeBridge();
		_renderer->SetWebMessageHandler([this](std::string_view a_viewId, std::string_view a_json) {
			if (_bridge) _bridge->HandleWebMessage(a_viewId, a_json);
		});
		if (!UiPass::Install()) {
			_osfSettings.ReportFailure("startup.draw-path", "webview.draw-path", "Scaleform UI pass hook failed");
			return false;
		}
		if (_developerMode && !_devViewReload) {
			_devViewReload = std::make_unique<DevViewReloadWorker>(Paths::ViewsDir(), [this](std::string_view a_id) {
				return _renderer && _renderer->RefreshViewFiles(a_id);
			});
		}
		_osfSettings.ClearFailure("startup.renderer");
		_osfSettings.ClearFailure("startup.compositor");
		_osfSettings.ClearFailure("startup.draw-path");
		_webRuntimeReady = true;
		REX::INFO("Runtime: lazy WebView2 runtime initialized");
		return true;
	}

	void Runtime::OnDataLoaded()
	{
		_dataLoadedInitPending.store(true, std::memory_order_release);
	}

	void Runtime::OnPostDataLoaded()
	{
		_postDataLoadedReady.store(true, std::memory_order_release);
	}

	void Runtime::InitializeDataLoadedState()
	{
		REX::DEBUG("Runtime: consuming kPostDataLoad work on the main-thread tick");
		API::Papyrus::Install();
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


	void Runtime::ApplyPresentationRequests(
		const std::vector<ViewRequestQueue::Operation>& a_local,
		const std::vector<API::BridgeApi::ViewPresentationRequest>& a_plugin)
	{
		if (a_local.empty() && a_plugin.empty()) {
			return;
		}
		for (const auto& operation : a_local) {
			if (const auto* pointer = std::get_if<ViewRequestQueue::RelativePointerRequest>(&operation)) {
				ApplyViewPresentationPolicy();
				ApplyRelativePointerRequests({ *pointer });
				continue;
			}
			if (const auto* open = std::get_if<ViewRequestQueue::OpenRequest>(&operation)) {
				BeginViewOpen(open->view, "on demand", open->requestedAt);
				continue;
			}
			switch (std::get<ViewPresentationRequest>(operation)) {
			case ViewPresentationRequest::Back: {
				const auto active = _presentation.ActiveMenu();
				if (_viewOpens.PendingMenu()) {
					_viewOpens.CancelMenu();
				} else if (active) {
					_viewOpens.CancelTiming(*active);
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
				_viewOpens.Clear();
				_presentation.CloseAll();
				break;
			}
		}
		for (const auto& r : a_plugin) {
			if (r.open) {
				BeginViewOpen(r.view, "on demand", r.requestedAt);
			} else {
				_viewOpens.Cancel(r.view);
				_presentation.Close(r.view);
			}
		}
		ApplyViewPresentationPolicy();
	}

	void Runtime::ApplyRelativePointerRequests(const std::vector<ViewRequestQueue::RelativePointerRequest>& a_requests)
	{
		for (const auto& request : a_requests) {
			if (!request.active) {
				_relativePointer.End(request.view);
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
			if (!_relativePointer.Begin(request.view) && _bridge) {
				_bridge->ReportProtocolFault(request.view, "pointer-capture-unavailable",
					"the native owner did not register a relative pointer handler", {}, false);
			}
		}
	}

	bool Runtime::BeginViewOpen(std::string_view a_id, std::string_view a_reason,
		std::optional<ViewOpenCoordinator::Clock::time_point> a_requestedAt)
	{
		const auto* manifest = _views.Find(a_id);
		if (!manifest) {
			REX::WARN("Runtime: cannot open '{}' — no discovered view has that id", a_id);
			_osfSettings.ReportFailure("view." + std::string(a_id), "view.not-found",
				"The requested OSF UI view is not installed", { { "view", a_id } });
			return false;
		}
		a_id = manifest->id;
		if (manifest->kind == ViewKind::Menu && MenuEventSink::TransitionOpen()) return false;
		if (!EnsureWebRuntime()) {
			REX::WARN("Runtime: cannot open '{}' — lazy WebView runtime initialization failed", a_id);
			return false;
		}
		// Require both installation and the lazy render-worker self-test before allowing input capture.
		if (!UiPass::DrawEnabled()) {
			REX::WARN("Runtime: cannot open '{}' — the Scaleform UI draw path is unavailable", a_id);
			_osfSettings.ReportFailure("view." + std::string(a_id), "view.draw-path-unavailable",
				"The view cannot open because the UI draw path is unavailable", { { "view", a_id } });
			return false;
		}
		if (_rendererFailed) {
			if (_browserHostRecovery.RequestManualRetry(_nowSeconds)) {
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
		if (_presentation.IsOpen(a_id) ||
			_viewOpens.Contains(a_id)) return false;
		const bool requiresCaptureIntegration = manifest->kind == ViewKind::Menu && manifest->capturesInput;
		if (requiresCaptureIntegration && !_inputCapture.IntegrationAttempted()) {
			_inputCapture.EnsureIntegration(_postDataLoadedReady.load(std::memory_order_acquire));
		}
		if (requiresCaptureIntegration && _inputCapture.IntegrationAttempted() &&
			!_inputCapture.IntegrationAvailable()) {
			REX::WARN("Runtime: cannot open '{}' — required input integration is unavailable", a_id);
			_osfSettings.ReportFailure("view." + std::string(a_id), "view.input-unavailable",
				"The view requires web input, but input integration is unavailable", { { "view", a_id } });
			return false;
		}

		const auto preflight = API::BridgeApi::Get().RunViewOpenPreflight(a_id);
		if (preflight == API::BridgeApi::ViewOpenPreflightResult::kDenied) {
			REX::WARN("Runtime: native view-open preflight denied '{}'; current presentation retained", a_id);
			return false;
		}
		const bool requiresStateBarrier = preflight == API::BridgeApi::ViewOpenPreflightResult::kAllowed;
		if (requiresStateBarrier) {
			REX::DEBUG("Runtime: native view-open preflight allowed '{}'; holding presentation through the next main tick", a_id);
		}

		if (manifest->kind == ViewKind::Menu &&
			m_viewLoads.GetState(a_id) != ViewLoadState::Finished) {
			_viewOpens.BeginTiming(a_id, _presentation.IsInstantiated(a_id), a_requestedAt);
		}
		if (!_presentation.IsInstantiated(a_id) && !InstantiateView(*manifest, a_reason)) {
			_viewOpens.CancelTiming(a_id);
			return false;
		}

		if (manifest->kind == ViewKind::Hud) {
			_viewOpens.QueueHud(a_id, _mainTickSerial + (requiresStateBarrier ? 1 : 0));
			return true;
		}
		if (_viewOpens.QueueMenu(a_id, ViewOpenReadiness(a_id), requiresStateBarrier, _mainTickSerial, a_requestedAt)) {
			return _presentation.Open(a_id);
		}
		REX::DEBUG("Runtime: holding open of '{}' until its load, input integration and retained-state barrier are ready", a_id);
		return true;
	}

	ViewOpenCoordinator::Readiness Runtime::ViewOpenReadiness(std::string_view a_id) const
	{
		using Readiness = ViewOpenCoordinator::Readiness;
		const auto* manifest = _views.Find(a_id);
		if (!manifest || !_presentation.IsInstantiated(a_id)) return Readiness::Missing;
		if (manifest->kind == ViewKind::Menu && manifest->capturesInput) {
			if (!_inputCapture.IntegrationAttempted()) return Readiness::WaitingForInput;
			if (!_inputCapture.IntegrationAvailable()) return Readiness::InputUnavailable;
		}
		return m_viewLoads.GetState(a_id) == ViewLoadState::Finished ? Readiness::Ready : Readiness::Loading;
	}

	void Runtime::DrivePendingOpen()
	{
		const bool menusAllowed = _inputCapture.MenuEventsAvailable() && !MenuEventSink::TransitionOpen();
		if (!_rendererFailed && menusAllowed && !_inputCapture.IntegrationAttempted() && _viewOpens.PendingMenu()) {
			const auto* manifest = _views.Find(*_viewOpens.PendingMenu());
			if (manifest && manifest->capturesInput) _inputCapture.EnsureIntegration(_postDataLoadedReady.load(std::memory_order_acquire));
		}
		const auto ready = _viewOpens.TakeReady(_mainTickSerial, !_rendererFailed, menusAllowed,
			[this](std::string_view a_id) { return ViewOpenReadiness(a_id); });
		bool changed = false;
		for (const auto& id : ready) {
			changed = _presentation.Open(id) || changed;
			REX::DEBUG("Runtime: open prerequisites completed; opening '{}'", id);
		}
		if (changed) ApplyViewPresentationPolicy();
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
			if (HudAutoStartEligible(*m)) {
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
		
		if (!UiPass::DrawEnabled() && _presentation.ActiveMenu()) {
			REX::WARN("Runtime: closing a requested menu because the Scaleform UI draw path is unavailable");
			_viewOpens.SuspendMenus();
			_presentation.CloseActiveMenu();
		}

		if (_presentation.DesiredCapture() && !_inputCapture.IntegrationAvailable()) {
			REX::WARN("Runtime: closing a requested menu because required input integration is unavailable");
			_viewOpens.SuspendMenus();
			_presentation.CloseActiveMenu();
		}

		// Block OSF hotkeys before publishing browser visibility or input capture.
		ReconcileInputSuppression();

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
				_osfSettings.ClearFailure("view." + layer.id);
				_renderer->SetViewHidden(layer.id, false);
			}
		}
		for (const auto& layer : layers) {
			if (layer.hidden) {
				_renderer->SetViewHidden(layer.id, true);
			}
		}

		const bool desiredCapture = _presentation.DesiredCapture();
		_relativePointer.ReconcileOwner(desiredCapture && active ? *active : std::string_view{});
		_inputCapture.PublishCapture(desiredCapture);

		const bool visible = _presentation.DesiredVisible();
		const bool wasVisible = m_visible.exchange(visible);
		if (visible && !wasVisible) {
			_renderer->SetPointerInputEnabled(false);
		}
		_inputCapture.ReconcileBrowserFocus(_renderer.get(), m_visible.load(), _presentation.ActiveMenu().has_value());
		if (!visible) {
			_renderer->SetPointerInputEnabled(true);
		}
		if (_compositor) {
			if (visible && !wasVisible) {
				m_viewReveal.Arm();
				_pointerInput.SuspendGeometry();
			} else {
				if (!visible) {
					m_viewReveal.Cancel();  // closed while a reveal was still pending
					_pointerInput.ResumeGeometry();
				}
				if (!m_viewReveal.Pending()) {
					_compositor->SetVisible(visible);
				}
			}
		}

		if (visible) {
			if (!wasVisible) {
				_pointerInput.CenterCursor();
			}
			if (active && _pointerInput.GeometryReady()) {
				_pointerInput.QueueMouseMove();  // flushed by Update's coalesced move injection
			}
		}

		const std::string shown = (visible && active) ? *active : std::string();
		if (shown != _lastShownView) {
			const std::string previous = _lastShownView;
			_lastShownView = shown;
			const char* reason = (visible == wasVisible) ? "focus" : "overlay";
			if (!previous.empty()) {
				API::BridgeApi::Get().DispatchViewLifecycle(previous, API::ViewLifecyclePhase::kHidden);
				if (_bridge) {
					_bridge->Emit(previous, "ui.visibility", nlohmann::json{ { "visible", false }, { "reason", reason } });
				}
			}
			if (!shown.empty()) {
				API::BridgeApi::Get().DispatchViewLifecycle(shown, API::ViewLifecyclePhase::kShown);
				if (_bridge) {
					_bridge->Emit(shown, "ui.visibility", nlohmann::json{ { "visible", true }, { "reason", reason } });
				}
			}
		}
		if (visible != wasVisible) {
			REX::INFO("Runtime: overlay visibility -> {} (capture={})", visible, _inputCapture.CaptureRequested());
		}

		BroadcastViewsData();
	}

	bool Runtime::IsVisible() const
	{
		return m_visible.load();
	}

	void Runtime::DriveBrowserHostRecovery()
	{
		_browserHostRecovery.ObserveHealth(_nowSeconds);
		if (_browserHostRecovery.ExpireResponseWait(_nowSeconds)) {
			REX::ERROR("Runtime: replacement browser host produced no load response in {:.0f}s", BrowserHostRecovery::kResponseTimeoutSeconds);
			if (_browserHostRecovery.PhaseValue() ==
				BrowserHostRecovery::Phase::Exhausted) {
				REX::ERROR("Runtime: automatic browser-host recovery exhausted; the next explicit menu open will start a fresh retry cycle");
			}
		}

		if (!_browserHostRecovery.BeginDueAttempt(_nowSeconds)) {
			return;
		}

		const auto attempt = _browserHostRecovery.Attempts();
		REX::INFO("Runtime: restarting browser host (attempt {}/{})", attempt, BrowserHostRecovery::kMaxAttempts);
		_renderer->RestartAfterFailure();

		_rendererFailureLatched = false;
		RehydrateRendererAfterRestart();
	}

	void Runtime::RehydrateRendererAfterRestart()
	{
		if (!_renderer || !_bridge) {
			return;
		}

		_relativePointer.Cancel();
		m_viewRecovery.ClearAll();
		m_viewInputGrants.ResetAll();
		_pointerInput.DiscardMouseMove();
		m_viewReveal.Reset();
		_inputCapture.ResetBrowserFocus();
		std::size_t reloaded = 0;
		for (const auto& manifest : _views.All()) {
			if (!_presentation.IsInstantiated(manifest.id)) {
				continue;
			}
			if (manifest.kind == ViewKind::Hud && _presentation.IsOpen(manifest.id)) {
				_presentation.Close(manifest.id);
				_viewOpens.QueueHud(manifest.id, _mainTickSerial);
			}
			NavigateView(manifest);
			++reloaded;
		}

		ApplyViewPresentationPolicy();
		BroadcastViewsData();
		REX::INFO("Runtime: replayed {} instantiated view(s) to the replacement browser host; menus stay closed and requested HUDs await load", reloaded);
	}

	void Runtime::OnRendererFailure(const WebView2HostWebRenderer::FailureEvent& a_event)
	{
		if (_rendererFailureLatched) {
			return;
		}
		API::BridgeApi::Get().SetBridgeAvailability(nullptr);
		_rendererFailureLatched = true;
		_rendererFailed = true;
		_osfSettings.ReportFailure("runtime.renderer", "webview.renderer-failed",
			"The OSF UI browser stopped working",
			{ { "view", a_event.viewId }, { "stage", a_event.stage }, { "detail", a_event.description }, { "errorCode", a_event.errorCode } });
		const bool retryableBrowserHostLoss =
			a_event.stage == "host-connection" && _renderer;
		if (retryableBrowserHostLoss) {
			_browserHostRecovery.OnRetryableFailure(_nowSeconds);
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

		_viewOpens.SuspendMenus();
		_presentation.CloseActiveMenu();
		_presentation.SetSuspended(true);
		ApplyViewPresentationPolicy();

		ReconcileFocusMenu();
		_inputCapture.ReconcileControlLayer(_presentation.DesiredCapture(), IsInputCaptured());
		SimPause::Apply(_presentation.DesiredPause());
		FreeCursor::Apply(false);
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
		const auto previousCapture = _pointerInput.CaptureSize();
		const auto previousView = _pointerInput.ViewportSize();
		const auto observedTarget = _compositor ?
			_compositor->GetObservedOutputSize() : std::nullopt;
		const auto observedWidth = observedTarget ? observedTarget->width : 0;
		const auto observedHeight = observedTarget ? observedTarget->height : 0;
		const bool captureChanged =
			output.width != previousCapture.width ||
			output.height != previousCapture.height;
		const bool viewportChanged =
			view.width != previousView.width || view.height != previousView.height;
		const bool modeChanged = _pointerInput.UpdateFixedScaleformGeometry(fixedScaleformGeometry);
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
			if (captureChanged) {
				m_viewReveal.ArmForResize();
			} else {
				m_viewReveal.Arm();
			}
			_pointerInput.SuspendGeometry();
		}
		_relativePointer.Cancel();
		_pointerInput.PublishGeometry(output, view);
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

	void Runtime::UpdateViewReveal()
	{
		if (!_initialized || !IsVisible() || !_renderer || !_compositor) {
			return;
		}

		const auto frame = _renderer->Frames()->Latest();
		const auto expected = _pointerInput.CaptureSize();
		const bool outputSizeKnown = _pointerInput.GameClientSizeObserved();
		std::optional<ViewRevealGate::FrameObservation> observation;
		if (frame) {
			observation = ViewRevealGate::FrameObservation{
				.generation = frame->ringGeneration,
				.index = frame->frameIndex,
				.outputSizeKnown = outputSizeKnown,
				.matchesExpectedSize = frame->width == expected.width && frame->height == expected.height,
			};
		}

		const auto decision = m_viewReveal.Observe(observation, _nowSeconds);
		if (decision.frameChanged && frame) {
			if (observation && observation->outputSizeKnown && observation->matchesExpectedSize) {
				if (const auto active = _presentation.ActiveMenu()) {
					if (const auto timing = _viewOpens.FinishTiming(*active)) {
						REX::INFO("Runtime: cold-open timing '{}': {} ms total (request->instantiate {} ms, instantiate->load {} ms, load->presentable-frame {} ms)",
							timing->view, timing->totalMs, timing->instantiateMs, timing->loadMs, timing->presentMs);
					}
				}
			}
		}
		if (decision.reveal) {
			_compositor->SetVisible(true);  // the cached frame is fresh and output-sized
			_pointerInput.ResumeGeometry();
			_renderer->SetPointerInputEnabled(true);
			_pointerInput.QueueMouseMove();
			return;
		}
		if (!decision.timedOut) {
			return;
		}

		const auto active = _presentation.ActiveMenu().value_or("<none>");
		REX::ERROR("Runtime: overlay reveal for '{}' produced no presentable frame in {:.1f}s - closing it and releasing input/pause state", active, decision.heldSeconds);
		OnRendererFailure({ .stage = "host-connection", .description = "overlay reveal timed out" });
	}

}
