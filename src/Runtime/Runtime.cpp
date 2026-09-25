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
		m_views.DiscoverAll(Paths::ViewsDir());

		std::vector<std::string> discoveredViewIds;
		discoveredViewIds.reserve(m_views.All().size());

		for (const auto& manifest : m_views.All()) {
			discoveredViewIds.push_back(manifest.id);
		}

		API::BridgeApi::Get().SetViewCatalog(discoveredViewIds);
	}

	bool Runtime::InitializeRenderer()
	{
		auto renderer = std::make_unique<WebView2HostWebRenderer>();

		const auto initialWidth = kDefaultViewWidth;
		const auto initialHeight = kDefaultViewHeight;

		m_pointerInput.Initialize({ initialWidth, initialHeight });

		WebView2HostConfig rendererConfig{
			.width = initialWidth,
			.height = initialHeight,
			.devMode = m_developerMode,
			.highRefreshCapture = m_highRefreshCapture,
			.language = m_osfSettings.Language(),
			.dataDir = Paths::DataDir(),
		};

		if (!renderer->Initialize(rendererConfig)) {
			REX::ERROR("Runtime: WebView2 renderer failed to initialize");
			return false;
		}

		m_renderer = std::move(renderer);
		return true;
	}

	void Runtime::WireRendererLifecycleCallbacks()
	{
		m_renderer->SetLoadHandler([this](const WebView2HostWebRenderer::LoadEvent& a_e) {
			OnViewLoad(a_e.viewId, a_e.failed, a_e.url, a_e.description, a_e.errorCode);
		});

		m_renderer->SetFailureHandler([this](const WebView2HostWebRenderer::FailureEvent& a_e) {
			OnRendererFailure(a_e);
		});

		m_renderer->SetCursorChangeHandler([](CursorShape a_shape) {
			HardwareCursor::SetShape(a_shape);
		});
	}

	bool Runtime::InitializeCompositor()
	{
		auto compositor = std::make_unique<D3D12Compositor>();
		if (!compositor->Initialize(m_renderer->Frames())) {
			REX::ERROR("Runtime: D3D12 compositor failed to initialize");
			return false;
		}
		m_compositor = std::move(compositor);
		return true;
	}

	void Runtime::InitializeBridge()
	{
		if (m_bridge) return;
		auto bridge = std::make_unique<MessageBridge>([this](std::string_view a_viewId, std::string_view a_json) {
			if (m_renderer) {
				m_renderer->SendMessageToWeb(a_viewId, a_json);
			}
		});
		
		bridge->SetHelloHook([this](std::string_view a_viewId) { OnViewGreeted(a_viewId); });

		bridge->SetProtocolFaultSink([this](std::string_view a_viewId, std::string_view a_code, std::string_view a_message,
			const nlohmann::json& a_detail, bool a_viewFault) {
			OnProtocolFault(a_viewId, a_code, a_message, a_detail, a_viewFault);
		});

		RegisterPlatformEndpoints(*bridge);
		m_bridge = std::move(bridge);
	}

    void Runtime::InitializeStartupViews()
    {
		std::size_t queued = 0;
		for (const auto& manifest : m_views.All()) {
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
		if (m_initialized) {
			return true;
		}
		m_rendererFailed = false;
		m_rendererFailureLatched = false;
		m_browserHostRecovery.Reset();

		if (!Paths::Initialize()) {
			return false;
		}

		m_initialized = true;
		REX::INFO("Runtime: add-on loaded; waiting for SFSE kPostPostLoad before acquiring OSF Settings");
		return true;
	}

	void Runtime::OnPostPostLoad()
	{
		if (m_postPostLoadAttempted) {
			return;
		}
		m_postPostLoadAttempted = true;
		if (!m_osfSettings.Initialize()) {
			REX::ERROR("Runtime: OSF Settings dependency unavailable or ABI-incompatible; OSF UI remains inert");
			return;
		}
		m_developerMode = m_osfSettings.DeveloperMode();
		m_highRefreshCapture = m_osfSettings.HighRefreshCapture();
		Log::SetDebugLogging(m_developerMode);
		LoadStartupContent();
		m_osfSettings.RegisterLaunchers(m_views.All());
		InitializeStartupViews();

		REX::INFO("Runtime: lightweight add-on ready; WebView2 will initialize on first view demand");
	}

	bool Runtime::EnsureWebRuntime()
	{
		if (m_webRuntimeReady) return true;
		if (m_webRuntimeInitializing || !m_osfSettings.Available()) return false;
		m_webRuntimeInitializing = true;
		struct ResetInitializing final {
			bool& flag;
			~ResetInitializing() { flag = false; }
		} resetInitializing{ m_webRuntimeInitializing };
		if (!m_renderer && !InitializeRenderer()) {
			m_osfSettings.ReportFailure("startup.renderer", "webview.renderer-init", "WebView2 renderer failed to initialize");
			return false;
		}
		WireRendererLifecycleCallbacks();
		if (!m_compositor && !InitializeCompositor()) {
			m_osfSettings.ReportFailure("startup.compositor", "webview.compositor-init", "D3D12 compositor failed to initialize");
			return false;
		}
		InitializeBridge();
		m_renderer->SetWebMessageHandler([this](std::string_view a_viewId, std::string_view a_json) {
			if (m_bridge) m_bridge->HandleWebMessage(a_viewId, a_json);
		});
		if (!UiPass::Install()) {
			m_osfSettings.ReportFailure("startup.draw-path", "webview.draw-path", "Scaleform UI pass hook failed");
			return false;
		}
		if (m_developerMode && !m_devViewReload) {
			m_devViewReload = std::make_unique<DevViewReloadWorker>(Paths::ViewsDir(), [this](std::string_view a_id) {
				return m_renderer && m_renderer->RefreshViewFiles(a_id);
			});
		}
		m_osfSettings.ClearFailure("startup.renderer");
		m_osfSettings.ClearFailure("startup.compositor");
		m_osfSettings.ClearFailure("startup.draw-path");
		m_webRuntimeReady = true;
		REX::INFO("Runtime: lazy WebView2 runtime initialized");
		return true;
	}

	void Runtime::OnDataLoaded()
	{
		m_dataLoadedInitPending.store(true, std::memory_order_release);
	}

	void Runtime::OnPostDataLoaded()
	{
		m_postDataLoadedReady.store(true, std::memory_order_release);
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
				const auto active = m_presentation.ActiveMenu();
				if (m_viewOpens.PendingMenu()) {
					m_viewOpens.CancelMenu();
				} else if (active) {
					m_viewOpens.CancelTiming(*active);
					if (const auto target = m_viewInputGrants.BackTargetFor(*active)) {
						BeginViewOpen(*target, "for native back navigation");
					} else if (m_viewInputGrants.OwnsBackAction(*active) && m_renderer) {
						constexpr std::uint32_t kVkEscape = 0x1B;
						m_renderer->InjectKeyEvent(kVkEscape, true);
						m_renderer->InjectKeyEvent(kVkEscape, false);
					} else {
						m_presentation.CloseActiveMenu();
					}
				} else {
					m_presentation.CloseActiveMenu();
				}
				break;
			}
			case ViewPresentationRequest::CloseAll:
				m_viewOpens.Clear();
				m_presentation.CloseAll();
				break;
			}
		}
		for (const auto& r : a_plugin) {
			if (r.open) {
				BeginViewOpen(r.view, "on demand", r.requestedAt);
			} else {
				m_viewOpens.Cancel(r.view);
				m_presentation.Close(r.view);
			}
		}
		ApplyViewPresentationPolicy();
	}

	void Runtime::ApplyRelativePointerRequests(const std::vector<ViewRequestQueue::RelativePointerRequest>& a_requests)
	{
		for (const auto& request : a_requests) {
			if (!request.active) {
				m_relativePointer.End(request.view);
				continue;
			}

			const auto active = m_presentation.ActiveMenu();
			if (!IsInputCaptured() || !active || *active != request.view || !m_presentation.IsOpen(request.view)) {
				if (m_bridge) {
					m_bridge->ReportProtocolFault(request.view, "pointer-capture-forbidden",
						"only the visible input-owning menu can capture relative pointer input");
				}
				continue;
			}
			if (!m_relativePointer.Begin(request.view) && m_bridge) {
				m_bridge->ReportProtocolFault(request.view, "pointer-capture-unavailable",
					"the native owner did not register a relative pointer handler", {}, false);
			}
		}
	}

	bool Runtime::BeginViewOpen(std::string_view a_id, std::string_view a_reason,
		std::optional<ViewOpenCoordinator::Clock::time_point> a_requestedAt)
	{
		const auto* manifest = m_views.Find(a_id);
		if (!manifest) {
			REX::WARN("Runtime: cannot open '{}' — no discovered view has that id", a_id);
			m_osfSettings.ReportFailure("view." + std::string(a_id), "view.not-found",
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
			m_osfSettings.ReportFailure("view." + std::string(a_id), "view.draw-path-unavailable",
				"The view cannot open because the UI draw path is unavailable", { { "view", a_id } });
			return false;
		}
		if (m_rendererFailed) {
			if (m_browserHostRecovery.RequestManualRetry(m_nowSeconds)) {
				REX::INFO("Runtime: open of '{}' requested a fresh browser-host recovery cycle; the overlay remains closed until the replacement reaches its reveal gate", a_id);
			} else if (m_browserHostRecovery.PhaseValue() ==
				BrowserHostRecovery::Phase::Waiting ||
				m_browserHostRecovery.PhaseValue() ==
				BrowserHostRecovery::Phase::AwaitingResponse) {
				REX::WARN("Runtime: cannot open '{}' yet - the browser host is recovering", a_id);
			} else {
				REX::WARN("Runtime: cannot open '{}' - the web renderer needs a game restart or "
					"the repair described in the log", a_id);
			}
			return false;
		}
		if (m_presentation.IsOpen(a_id) ||
			m_viewOpens.Contains(a_id)) return false;
		const bool requiresCaptureIntegration = manifest->kind == ViewKind::Menu && manifest->capturesInput;
		if (requiresCaptureIntegration && !m_inputCapture.IntegrationAttempted()) {
			m_inputCapture.EnsureIntegration(m_postDataLoadedReady.load(std::memory_order_acquire));
		}
		if (requiresCaptureIntegration && m_inputCapture.IntegrationAttempted() &&
			!m_inputCapture.IntegrationAvailable()) {
			REX::WARN("Runtime: cannot open '{}' — required input integration is unavailable", a_id);
			m_osfSettings.ReportFailure("view." + std::string(a_id), "view.input-unavailable",
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
			m_viewOpens.BeginTiming(a_id, m_presentation.IsInstantiated(a_id), a_requestedAt);
		}
		if (!m_presentation.IsInstantiated(a_id) && !InstantiateView(*manifest, a_reason)) {
			m_viewOpens.CancelTiming(a_id);
			return false;
		}

		if (manifest->kind == ViewKind::Hud) {
			m_viewOpens.QueueHud(a_id, m_mainTickSerial + (requiresStateBarrier ? 1 : 0));
			return true;
		}
		if (m_viewOpens.QueueMenu(a_id, ViewOpenReadiness(a_id), requiresStateBarrier, m_mainTickSerial, a_requestedAt)) {
			return m_presentation.Open(a_id);
		}
		REX::DEBUG("Runtime: holding open of '{}' until its load, input integration and retained-state barrier are ready", a_id);
		return true;
	}

	ViewOpenCoordinator::Readiness Runtime::ViewOpenReadiness(std::string_view a_id) const
	{
		using Readiness = ViewOpenCoordinator::Readiness;
		const auto* manifest = m_views.Find(a_id);
		if (!manifest || !m_presentation.IsInstantiated(a_id)) return Readiness::Missing;
		if (manifest->kind == ViewKind::Menu && manifest->capturesInput) {
			if (!m_inputCapture.IntegrationAttempted()) return Readiness::WaitingForInput;
			if (!m_inputCapture.IntegrationAvailable()) return Readiness::InputUnavailable;
		}
		return m_viewLoads.GetState(a_id) == ViewLoadState::Finished ? Readiness::Ready : Readiness::Loading;
	}

	void Runtime::DrivePendingOpen()
	{
		const bool menusAllowed = m_inputCapture.MenuEventsAvailable() && !MenuEventSink::TransitionOpen();
		if (!m_rendererFailed && menusAllowed && !m_inputCapture.IntegrationAttempted() && m_viewOpens.PendingMenu()) {
			const auto* manifest = m_views.Find(*m_viewOpens.PendingMenu());
			if (manifest && manifest->capturesInput) m_inputCapture.EnsureIntegration(m_postDataLoadedReady.load(std::memory_order_acquire));
		}
		const auto ready = m_viewOpens.TakeReady(m_mainTickSerial, !m_rendererFailed, menusAllowed,
			[this](std::string_view a_id) { return ViewOpenReadiness(a_id); });
		bool changed = false;
		for (const auto& id : ready) {
			changed = m_presentation.Open(id) || changed;
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
			if (m_presentation.IsInstantiated(id)) {
				REX::DEBUG("Runtime: plugin RegisterView('{}') — already instantiated, left untouched", id);
				continue;
			}
			const auto* m = m_views.Find(id);
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
		if (!m_renderer) {
			return;
		}
		
		if (!UiPass::DrawEnabled() && m_presentation.ActiveMenu()) {
			REX::WARN("Runtime: closing a requested menu because the Scaleform UI draw path is unavailable");
			m_viewOpens.SuspendMenus();
			m_presentation.CloseActiveMenu();
		}

		if (m_presentation.DesiredCapture() && !m_inputCapture.IntegrationAvailable()) {
			REX::WARN("Runtime: closing a requested menu because required input integration is unavailable");
			m_viewOpens.SuspendMenus();
			m_presentation.CloseActiveMenu();
		}

		// Block OSF hotkeys before publishing browser visibility or input capture.
		ReconcileInputSuppression();

		const auto layers = m_presentation.DesiredLayers();
		for (const auto& layer : layers) {
			m_renderer->SetViewOrder(layer.id, layer.z);
		}
		const auto active = m_presentation.ActiveMenu();
		// Publish the input target before its view is shown. The browser host then
		// grants focus only after that target becomes visible.
		if (active) {
			m_renderer->SetInputTargetView(*active);
		}
		// A menu switch is intentionally show-before-hide. The browser host keeps the outgoing visual until the incoming view passes its paint handshake;
		for (const auto& layer : layers) {
			if (!layer.hidden) {
				m_osfSettings.ClearFailure("view." + layer.id);
				m_renderer->SetViewHidden(layer.id, false);
			}
		}
		for (const auto& layer : layers) {
			if (layer.hidden) {
				m_renderer->SetViewHidden(layer.id, true);
			}
		}

		const bool desiredCapture = m_presentation.DesiredCapture();
		m_relativePointer.ReconcileOwner(desiredCapture && active ? *active : std::string_view{});
		m_inputCapture.PublishCapture(desiredCapture);

		const bool visible = m_presentation.DesiredVisible();
		const bool wasVisible = m_visible.exchange(visible);
		if (visible && !wasVisible) {
			m_renderer->SetPointerInputEnabled(false);
		}
		m_inputCapture.ReconcileBrowserFocus(m_renderer.get(), m_visible.load(), m_presentation.ActiveMenu().has_value());
		if (!visible) {
			m_renderer->SetPointerInputEnabled(true);
		}
		if (m_compositor) {
			if (visible && !wasVisible) {
				m_viewReveal.Arm();
				m_pointerInput.SuspendGeometry();
			} else {
				if (!visible) {
					m_viewReveal.Cancel();  // closed while a reveal was still pending
					m_pointerInput.ResumeGeometry();
				}
				if (!m_viewReveal.Pending()) {
					m_compositor->SetVisible(visible);
				}
			}
		}

		if (visible) {
			if (!wasVisible) {
				m_pointerInput.CenterCursor();
			}
			if (active && m_pointerInput.GeometryReady()) {
				m_pointerInput.QueueMouseMove();  // flushed by Update's coalesced move injection
			}
		}

		const std::string shown = (visible && active) ? *active : std::string();
		if (shown != m_lastShownView) {
			const std::string previous = m_lastShownView;
			m_lastShownView = shown;
			const char* reason = (visible == wasVisible) ? "focus" : "overlay";
			if (!previous.empty()) {
				API::BridgeApi::Get().DispatchViewLifecycle(previous, API::ViewLifecyclePhase::kHidden);
				if (m_bridge) {
					m_bridge->Emit(previous, "ui.visibility", nlohmann::json{ { "visible", false }, { "reason", reason } });
				}
			}
			if (!shown.empty()) {
				API::BridgeApi::Get().DispatchViewLifecycle(shown, API::ViewLifecyclePhase::kShown);
				if (m_bridge) {
					m_bridge->Emit(shown, "ui.visibility", nlohmann::json{ { "visible", true }, { "reason", reason } });
				}
			}
		}
		if (visible != wasVisible) {
			REX::INFO("Runtime: overlay visibility -> {} (capture={})", visible, m_inputCapture.CaptureRequested());
		}

		BroadcastViewsData();
	}

	bool Runtime::IsVisible() const
	{
		return m_visible.load();
	}

	void Runtime::DriveBrowserHostRecovery()
	{
		m_browserHostRecovery.ObserveHealth(m_nowSeconds);
		if (m_browserHostRecovery.ExpireResponseWait(m_nowSeconds)) {
			REX::ERROR("Runtime: replacement browser host produced no load response in {:.0f}s", BrowserHostRecovery::kResponseTimeoutSeconds);
			if (m_browserHostRecovery.PhaseValue() ==
				BrowserHostRecovery::Phase::Exhausted) {
				REX::ERROR("Runtime: automatic browser-host recovery exhausted; the next explicit menu open will start a fresh retry cycle");
			}
		}

		if (!m_browserHostRecovery.BeginDueAttempt(m_nowSeconds)) {
			return;
		}

		const auto attempt = m_browserHostRecovery.Attempts();
		REX::INFO("Runtime: restarting browser host (attempt {}/{})", attempt, BrowserHostRecovery::kMaxAttempts);
		m_renderer->RestartAfterFailure();

		m_rendererFailureLatched = false;
		RehydrateRendererAfterRestart();
	}

	void Runtime::RehydrateRendererAfterRestart()
	{
		if (!m_renderer || !m_bridge) {
			return;
		}

		m_relativePointer.Cancel();
		m_viewRecovery.ClearAll();
		m_viewInputGrants.ResetAll();
		m_pointerInput.DiscardMouseMove();
		m_viewReveal.Reset();
		m_inputCapture.ResetBrowserFocus();
		std::size_t reloaded = 0;
		for (const auto& manifest : m_views.All()) {
			if (!m_presentation.IsInstantiated(manifest.id)) {
				continue;
			}
			if (manifest.kind == ViewKind::Hud && m_presentation.IsOpen(manifest.id)) {
				m_presentation.Close(manifest.id);
				m_viewOpens.QueueHud(manifest.id, m_mainTickSerial);
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
		if (m_rendererFailureLatched) {
			return;
		}
		API::BridgeApi::Get().SetBridgeAvailability(nullptr);
		m_rendererFailureLatched = true;
		m_rendererFailed = true;
		m_osfSettings.ReportFailure("runtime.renderer", "webview.renderer-failed",
			"The OSF UI browser stopped working",
			{ { "view", a_event.viewId }, { "stage", a_event.stage }, { "detail", a_event.description }, { "errorCode", a_event.errorCode } });
		const bool retryableBrowserHostLoss =
			a_event.stage == "host-connection" && m_renderer;
		if (retryableBrowserHostLoss) {
			m_browserHostRecovery.OnRetryableFailure(m_nowSeconds);
			REX::ERROR("Runtime: browser-host connection failed for view '{}' (0x{:08X}): {} - closing the overlay; bounded browser-host recovery is scheduled", a_event.viewId, a_event.errorCode, a_event.description);
			if (m_browserHostRecovery.PhaseValue() ==
				BrowserHostRecovery::Phase::Exhausted) {
				REX::ERROR("Runtime: automatic browser-host recovery exhausted; the next explicit menu open will start a fresh retry cycle");
			}
		} else {
			m_browserHostRecovery.Disable();
			REX::ERROR("Runtime: renderer failed at '{}' for view '{}' (0x{:08X}): {} - closing the overlay and disabling it for this session", a_event.stage, a_event.viewId, a_event.errorCode, a_event.description);
		}
		m_viewRecovery.ClearAll();

		m_viewOpens.SuspendMenus();
		m_presentation.CloseActiveMenu();
		m_presentation.SetSuspended(true);
		ApplyViewPresentationPolicy();

		ReconcileFocusMenu();
		m_inputCapture.ReconcileControlLayer(m_presentation.DesiredCapture(), IsInputCaptured());
		SimPause::Apply(m_presentation.DesiredPause());
		FreeCursor::Apply(false);
	}

	void Runtime::OnOutputResized(std::uint32_t a_width, std::uint32_t a_height)
	{
		if (a_width == 0 || a_height == 0 || !m_renderer) {
			return;
		}
		const bool fixedScaleformGeometry =
			MenuEventSink::ChargenOpen() && UiPass::UsesScaleformEnd();
		const auto view = ViewSizeForOutput(
			{ .width = a_width, .height = a_height }, fixedScaleformGeometry);
		UiPass::SetExpectedOutputSize(a_width, a_height);

		const ViewSize output{ .width = a_width, .height = a_height };
		const auto previousCapture = m_pointerInput.CaptureSize();
		const auto previousView = m_pointerInput.ViewportSize();
		const auto observedTarget = m_compositor ?
			m_compositor->GetObservedOutputSize() : std::nullopt;
		const auto observedWidth = observedTarget ? observedTarget->width : 0;
		const auto observedHeight = observedTarget ? observedTarget->height : 0;
		const bool captureChanged =
			output.width != previousCapture.width ||
			output.height != previousCapture.height;
		const bool viewportChanged =
			view.width != previousView.width || view.height != previousView.height;
		const bool modeChanged = m_pointerInput.UpdateFixedScaleformGeometry(fixedScaleformGeometry);
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
		if (visible && m_compositor) {
			m_compositor->SetVisible(false);
			m_renderer->SetPointerInputEnabled(false);
			if (captureChanged) {
				m_viewReveal.ArmForResize();
			} else {
				m_viewReveal.Arm();
			}
			m_pointerInput.SuspendGeometry();
		}
		m_relativePointer.Cancel();
		m_pointerInput.PublishGeometry(output, view);
		if (captureChanged) {
			m_renderer->Resize(output.width, output.height);
		}
		if (viewportChanged) {
			m_renderer->SetViewport(view.width, view.height);
		}
		REX::INFO("Runtime: Scaleform geometry mode -> {} (ChargenMenu={}, handoff={}, capture {}x{}, viewport {}x{}, last target {}x{})",
			fixedScaleformGeometry ? "fixed-16:9" : "full-output",
			MenuEventSink::ChargenOpen(),
			UiPass::UsesScaleformEnd() ? "ScaleformEnd" : "post-composite",
			a_width, a_height, view.width, view.height, observedWidth, observedHeight);
	}

	void Runtime::UpdateViewReveal()
	{
		if (!m_initialized || !IsVisible() || !m_renderer || !m_compositor) {
			return;
		}

		const auto frame = m_renderer->Frames()->Latest();
		const auto expected = m_pointerInput.CaptureSize();
		const bool outputSizeKnown = m_pointerInput.GameClientSizeObserved();
		std::optional<ViewRevealGate::FrameObservation> observation;
		if (frame) {
			observation = ViewRevealGate::FrameObservation{
				.generation = frame->ringGeneration,
				.index = frame->frameIndex,
				.outputSizeKnown = outputSizeKnown,
				.matchesExpectedSize = frame->width == expected.width && frame->height == expected.height,
			};
		}

		const auto decision = m_viewReveal.Observe(observation, m_nowSeconds);
		if (decision.frameChanged && frame) {
			if (observation && observation->outputSizeKnown && observation->matchesExpectedSize) {
				if (const auto active = m_presentation.ActiveMenu()) {
					if (const auto timing = m_viewOpens.FinishTiming(*active)) {
						REX::INFO("Runtime: cold-open timing '{}': {} ms total (request->instantiate {} ms, instantiate->load {} ms, load->presentable-frame {} ms)",
							timing->view, timing->totalMs, timing->instantiateMs, timing->loadMs, timing->presentMs);
					}
				}
			}
		}
		if (decision.reveal) {
			m_compositor->SetVisible(true);  // the cached frame is fresh and output-sized
			m_pointerInput.ResumeGeometry();
			m_renderer->SetPointerInputEnabled(true);
			m_pointerInput.QueueMouseMove();
			return;
		}
		if (!decision.timedOut) {
			return;
		}

		const auto active = m_presentation.ActiveMenu().value_or("<none>");
		REX::ERROR("Runtime: overlay reveal for '{}' produced no presentable frame in {:.1f}s - closing it and releasing input/pause state", active, decision.heldSeconds);
		OnRendererFailure({ .stage = "host-connection", .description = "overlay reveal timed out" });
	}

}
