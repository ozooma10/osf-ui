#include "Runtime/Runtime.h"

#include <limits>

#include "API/BridgeApi.h"
#include "API/PapyrusApi.h"
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
			.resolveLanguage = [this] { return m_osfSettings.Language(); },
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

		bridge->SetProtocolFaultSink([this](std::string_view a_viewId, std::string_view a_code, std::string_view a_message, const nlohmann::json& a_detail, bool a_viewFault) {
			OnProtocolFault(a_viewId, a_code, a_message, a_detail, a_viewFault);
		});

		RegisterPlatformEndpoints(*bridge);
		m_bridge = std::move(bridge);
		API::BridgeApi::Get().AttachBridge(*m_bridge);
	}

    void Runtime::InitializeStartupViews()
    {
		std::size_t queued = 0;
		for (const auto& manifest : m_views.All()) {
			if (!HudAutoStartEligible(manifest)) {
				continue;
			}
			EnqueueOpenView(manifest.id);
			++queued;
		}
		REX::INFO("Runtime: queued {} manifest-selected HUD view(s) for startup", queued);
    }

    bool Runtime::Initialize()
	{
		if (m_initialized) {
			return true;
		}
		m_browserHostRecovery.Reset();

		if (!Paths::Initialize()) {
			return false;
		}
		LoadStartupContent();
		InitializeBridge();

		m_initialized = true;
		REX::INFO("Runtime: add-on loaded; waiting for SFSE kPostLoad before acquiring OSF Settings");
		return true;
	}

	void Runtime::OnPostLoad()
	{
		if (!m_osfSettings.Initialize()) {
			REX::ERROR("Runtime: OSF Settings dependency unavailable or ABI-incompatible; OSF UI remains inert");
			return;
		}
		m_developerMode = m_osfSettings.DeveloperMode();
		Log::SetDebugLogging(m_developerMode);
		m_osfSettings.RegisterLaunchers(m_views.All());
		if (!InitializeWebRuntime()) {
			REX::ERROR("Runtime: web runtime preparation failed; views are unavailable this session");
			return;
		}
		InitializeStartupViews();

		REX::INFO("Runtime: add-on prepared; browser host will start on view demand once language is ready");
	}

	bool Runtime::InitializeWebRuntime()
	{
		if (!InitializeRenderer()) {
			m_osfSettings.ReportFailure("startup.renderer", "webview.renderer-init", "WebView2 renderer failed to initialize");
			return false;
		}
		WireRendererLifecycleCallbacks();
		if (!InitializeCompositor()) {
			m_osfSettings.ReportFailure("startup.compositor", "webview.compositor-init", "D3D12 compositor failed to initialize");
			return false;
		}
		m_renderer->SetWebMessageHandler([this](std::string_view a_viewId, std::string_view a_json) {
			if (m_bridge) m_bridge->HandleWebMessage(a_viewId, a_json);
		});
		if (m_developerMode && !m_devViewReload) {
			m_devViewReload = std::make_unique<DevViewReloadWorker>(Paths::ViewsDir(), [this](std::string_view a_mod) {
				return m_renderer && m_renderer->RefreshModFiles(a_mod);
			});
			UpdateDevViewReloadMods();
		}
		m_osfSettings.ClearFailure("startup.renderer");
		m_osfSettings.ClearFailure("startup.compositor");
		m_osfSettings.ClearFailure("startup.draw-path");
		m_webRuntimeReady = true;
		REX::INFO("Runtime: web runtime prepared without launching the browser host");
		return true;
	}

	void Runtime::OnPostPostDataLoad()
	{
		m_engineIntegrationPending.store(true, std::memory_order_release);
	}

	void Runtime::InitializeEngineIntegration()
	{
		REX::DEBUG("Runtime: consuming kPostPostDataLoad work on the post-menu-advance runtime tick");
		API::Papyrus::Install();
		if (!m_inputCapture.Initialize()) {
			m_osfSettings.ReportFailure("startup.input", "input.unavailable", "Game UI input integration failed");
		}
	}

	void Runtime::EnqueuePresentationRequest(ViewPresentationRequest a_req)
	{
		API::BridgeApi::Get().ViewRequests().Enqueue(a_req);
	}

	void Runtime::EnqueueOpenView(std::string a_viewId)
	{
		API::BridgeApi::Get().ViewRequests().EnqueueView(std::move(a_viewId), true);
	}

	void Runtime::EnqueueCloseView(std::string a_viewId)
	{
		API::BridgeApi::Get().ViewRequests().EnqueueView(std::move(a_viewId), false);
	}

	void Runtime::EnqueueRelativePointerCapture(std::string a_viewId, bool a_active)
	{
		API::BridgeApi::Get().ViewRequests().EnqueueRelativePointer(std::move(a_viewId), a_active);
	}


	void Runtime::ApplyPresentationRequests(const std::vector<ViewRequestQueue::Operation>& a_requests)
	{
		for (const auto& operation : a_requests) {
			if (const auto* pointer = std::get_if<ViewRequestQueue::RelativePointerRequest>(&operation)) {
				// Pointer ownership observes all preceding presentation requests.
				CommitPresentation();
				ApplyRelativePointerRequest(*pointer);
				continue;
			}
			if (const auto* view = std::get_if<ViewRequestQueue::ViewRequest>(&operation)) {
				if (view->open) {
					PrepareViewOpen(view->view);
				} else {
					m_viewOpens.Cancel(view->view);
					m_presentation.Close(view->view);
				}
				continue;
			}
			switch (std::get<ViewPresentationRequest>(operation)) {
			case ViewPresentationRequest::Back: {
				const auto active = m_presentation.ActiveMenu();
				if (m_viewOpens.PendingMenu()) {
					m_viewOpens.CancelMenu();
				} else if (active) {
					if (const auto target = m_viewInputGrants.BackTargetFor(*active)) {
						PrepareViewOpen(*target, "for native back navigation");
					} else if (m_viewInputGrants.OwnsBackAction(*active) && m_renderer) {
						// Deliver Back to the menu selected by preceding requests.
						CommitPresentation();
						if (m_presentation.ActiveMenu() != active) continue;
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
	}

	void Runtime::ApplyRelativePointerRequest(const ViewRequestQueue::RelativePointerRequest& a_request)
	{
		if (!a_request.active) {
			m_relativePointer.End(a_request.view);
			return;
		}

		const auto active = m_presentation.ActiveMenu();
		if (!IsInputCaptured() || !active || *active != a_request.view || !m_presentation.IsOpen(a_request.view)) {
			if (m_bridge) {
				m_bridge->ReportProtocolFault(a_request.view, "pointer-capture-forbidden",
					"only the visible input-owning menu can capture relative pointer input");
			}
			return;
		}
		if (!m_relativePointer.Begin(a_request.view) && m_bridge) {
			m_bridge->ReportProtocolFault(a_request.view, "pointer-capture-unavailable",
				"the native owner did not register a relative pointer handler", {}, false);
		}
	}

	void Runtime::PrepareViewOpen(std::string_view a_id, std::string_view a_reason)
	{
		const auto* manifest = m_views.Find(a_id);
		if (!manifest) {
			REX::WARN("Runtime: cannot open '{}' — no discovered view has that id", a_id);
			m_osfSettings.ReportFailure("view." + std::string(a_id), "view.not-found", "The requested OSF UI view is not installed", { { "view", a_id } });
			return;
		}
		a_id = manifest->id;
		if (manifest->kind == ViewKind::Menu && MenuEventSink::TransitionOpen()) return;
		if (!m_webRuntimeReady) {
			REX::WARN("Runtime: cannot open '{}' — web runtime preparation failed", a_id);
			return;
		}
		if (!m_browserHostRecovery.IsAvailable()) {
			if (m_browserHostRecovery.RequestManualRetry(m_nowSeconds)) {
				REX::INFO("Runtime: open of '{}' requested a fresh browser-host recovery cycle; the overlay remains closed until the replacement reaches its reveal gate", a_id);
			} else if (m_browserHostRecovery.PhaseValue() ==
				BrowserHostRecovery::Phase::Waiting ||
				m_browserHostRecovery.PhaseValue() ==
				BrowserHostRecovery::Phase::AwaitingResponse) {
				REX::WARN("Runtime: cannot open '{}' yet - the browser host is recovering", a_id);
			} else {
				REX::WARN("Runtime: cannot open '{}' - the web renderer needs a game restart or the repair described in the log", a_id);
			}
			return;
		}
		if (m_presentation.IsOpen(a_id) ||
			m_viewOpens.Contains(a_id)) return;
		const bool requiresCaptureIntegration = manifest->kind == ViewKind::Menu && manifest->capturesInput;
		if (requiresCaptureIntegration && m_inputCapture.IntegrationAttempted() &&
			!m_inputCapture.IntegrationAvailable()) {
			REX::WARN("Runtime: cannot open '{}' — required input integration is unavailable", a_id);
			m_osfSettings.ReportFailure("view." + std::string(a_id), "view.input-unavailable", "The view requires web input, but input integration is unavailable", { { "view", a_id } });
			return;
		}

		// Creation stays hidden until CommitPresentation applies this intent.

		if (!InstantiateView(*manifest, a_reason)) return;

		if (manifest->kind == ViewKind::Hud) {
			m_viewOpens.QueueHud(a_id);
			return;
		}
		if (m_viewOpens.QueueMenu(a_id, ViewOpenReadiness(a_id))) {
			m_presentation.Open(a_id);
			return;
		}
		REX::DEBUG("Runtime: holding open of '{}' until its load and input integration are ready", a_id);
	}

	ViewOpenCoordinator::Readiness Runtime::ViewOpenReadiness(std::string_view a_id) const
	{
		using Readiness = ViewOpenCoordinator::Readiness;
		const auto* manifest = m_views.Find(a_id);
		if (!manifest || !m_presentation.IsInstantiated(a_id)) return Readiness::Missing;
		if (manifest->kind == ViewKind::Menu && m_presentation.Suspended()) return Readiness::Suspended;
		if (manifest->kind == ViewKind::Menu && manifest->capturesInput) {
			if (!m_inputCapture.IntegrationAttempted()) return Readiness::WaitingForInput;
			if (!m_inputCapture.IntegrationAvailable()) return Readiness::InputUnavailable;
		}
		return m_viewLoads.GetState(a_id) == ViewLoadState::Finished ? Readiness::Ready : Readiness::Loading;
	}

	void Runtime::DrivePendingOpen()
	{
		const auto ready = m_viewOpens.TakeReady(
			[this](std::string_view a_id) { return ViewOpenReadiness(a_id); });
		for (const auto& id : ready) {
			m_presentation.Open(id);
			REX::DEBUG("Runtime: open prerequisites completed; opening '{}'", id);
		}
	}

	void Runtime::DrainViewRegistrations(const std::vector<std::string>& a_ids)
	{
		if (a_ids.empty()) {
			return;
		}
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
				PrepareViewOpen(id, "via plugin RegisterView openOnStart");
			} else {
				// Discovery catalogues the view; registration validates intent without creating the page.
				REX::DEBUG("Runtime: plugin RegisterView('{}') accepted; creation deferred until first open", id);
			}
		}
	}

	void Runtime::ApplyViewPresentationPolicy()
	{
		if (!m_renderer) {
			return;
		}

		if (m_presentation.DesiredCapture() && !m_inputCapture.IntegrationAvailable()) {
			REX::WARN("Runtime: closing a requested menu because required input integration is unavailable");
			m_viewOpens.CancelMenu();
			m_presentation.CloseActiveMenu();
		}

		// Block OSF hotkeys before publishing browser visibility or input capture.
		ReconcileInputSuppression();
		if (!m_presentation.TakeChanged()) {
			ReconcileFrameState();
			return;
		}

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
		// Send show requests before hide requests. The browser host hides outgoing views immediately.
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

		// Engine capture and pause are settled before notifying native owners.
		ReconcileFrameState();
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
		m_viewReveal.Cancel();
		m_inputCapture.ResetBrowserFocus();
		std::size_t reloaded = 0;
		for (const auto& manifest : m_views.All()) {
			if (!m_presentation.IsInstantiated(manifest.id)) {
				continue;
			}
			if (manifest.kind == ViewKind::Hud && m_presentation.IsOpen(manifest.id)) {
				m_presentation.Close(manifest.id);
				m_viewOpens.QueueHud(manifest.id);
			}
			NavigateView(manifest);
			++reloaded;
		}

		m_presentation.Invalidate(); // the replacement host needs the complete policy
		REX::INFO("Runtime: replayed {} instantiated view(s) to the replacement browser host; menus stay closed and requested HUDs await load", reloaded);
	}

	void Runtime::OnRendererFailure(const WebView2HostWebRenderer::FailureEvent& a_event)
	{
		const bool retryableBrowserHostLoss =
			a_event.stage == "host-connection" && m_renderer;
		if (!m_browserHostRecovery.OnFailure(m_nowSeconds, retryableBrowserHostLoss)) {
			return;
		}
		API::BridgeApi::Get().SetBridgeAvailability(false);
		m_osfSettings.ReportFailure("runtime.renderer", "webview.renderer-failed",
			"The OSF UI browser stopped working",
			{ { "view", a_event.viewId }, { "stage", a_event.stage }, { "detail", a_event.description }, { "errorCode", a_event.errorCode } });
		if (retryableBrowserHostLoss) {
			REX::ERROR("Runtime: browser-host connection failed for view '{}' (0x{:08X}): {} - closing the overlay; bounded browser-host recovery is scheduled", a_event.viewId, a_event.errorCode, a_event.description);
			if (m_browserHostRecovery.PhaseValue() ==
				BrowserHostRecovery::Phase::Exhausted) {
				REX::ERROR("Runtime: automatic browser-host recovery exhausted; the next explicit menu open will start a fresh retry cycle");
			}
		} else {
			REX::ERROR("Runtime: renderer failed at '{}' for view '{}' (0x{:08X}): {} - closing the overlay and disabling it for this session", a_event.stage, a_event.viewId, a_event.errorCode, a_event.description);
		}
		m_viewRecovery.ClearAll();

		m_viewOpens.CancelMenu();
		m_presentation.CloseActiveMenu();
		m_presentation.SetSuspended(true);
		// Failure is an immediate release boundary, with no new opens or state drain.
		ApplyViewPresentationPolicy();
	}

	void Runtime::OnOutputResized(std::uint32_t a_width, std::uint32_t a_height)
	{
		if (a_width == 0 || a_height == 0 || !m_renderer) {
			return;
		}
		const bool fixedScaleformGeometry = MenuEventSink::ChargenOpen();
		const auto view = ViewSizeForOutput(
			{ .width = a_width, .height = a_height }, fixedScaleformGeometry);

		const ViewSize output{ .width = a_width, .height = a_height };
		const auto previousCapture = m_pointerInput.CaptureSize();
		const auto previousView = m_pointerInput.ViewportSize();
		const bool captureChanged =
			output.width != previousCapture.width ||
			output.height != previousCapture.height;
		const bool viewportChanged =
			view.width != previousView.width || view.height != previousView.height;
		if (!captureChanged && !viewportChanged) {
			return;
		}

		const bool visible = IsVisible();
		if (visible && m_compositor) {
			m_compositor->SetVisible(false);
			m_renderer->SetPointerInputEnabled(false);
			m_viewReveal.Arm();
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
		REX::INFO("Runtime: Scaleform geometry mode -> {} (ChargenMenu={}, capture {}x{}, viewport {}x{})",
			fixedScaleformGeometry ? "fixed-16:9" : "full-output",
			MenuEventSink::ChargenOpen(),
			a_width, a_height, view.width, view.height);
	}

	void Runtime::UpdateViewReveal()
	{
		if (!m_initialized || !IsVisible() || !m_renderer || !m_compositor || !m_viewReveal.Pending()) {
			return;
		}

		// Every arm site invalidated the cached frame, so any frame present now is fresh.
		const auto frame = m_renderer->Frames()->Latest();
		const auto expected = m_pointerInput.CaptureSize();
		const bool frameReady = frame && frame->width == expected.width && frame->height == expected.height;

		const auto decision = m_viewReveal.Observe(frameReady, m_nowSeconds);
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
