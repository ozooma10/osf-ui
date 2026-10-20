#include "Compat/V1/PapyrusAdapter.h"
#include "Runtime/Runtime.h"

#include "API/PapyrusApi.h"
#include "Compat/V1/LegacyBridge.h"
#include "Input/FocusMenu.h"
#include "Input/FreeCursor.h"
#include "Input/SimPause.h"
#include "Input/MenuEventSink.h"
#include "Input/OverlayInputHook.h"

namespace OSFUI
{
	void Runtime::ProcessLifecycleWork()
	{
		if (m_engineIntegrationPending.exchange(false, std::memory_order_acq_rel)) {
			InitializeEngineIntegration();
		}
		// Check once per update whether menus can open.
		const bool suspend = !m_inputCapture.MenuEventsAvailable() || MenuEventSink::TransitionOpen() || !m_browserHostRecovery.IsAvailable();
		if (m_presentation.SetSuspended(suspend) && suspend) {
			FailPendingOpen("The game is changing menus.");
		}
	}

	void Runtime::ProcessBackendState(const API::Papyrus::PendingBatch& a_papyrus, const std::vector<API::BridgeApi::ViewStateOp>& a_bridgeState)
	{
		// Save state even before a view exists; replay it when the page connects.
		if (a_papyrus.sessionReset) {
			m_retainedState.ClearSessionScoped();
			SurfaceService::Get().FinishInteraction("game-load");
		}
		for (const auto& state : a_papyrus.states) {
			m_retainedState.Set(state.mod, state.key, state.value, true);
			PublishModState(state.mod, state.key, state.value);
		}
		ApplyNativeState(a_bridgeState);
	}

	void Runtime::ApplyNativeState(const std::vector<API::BridgeApi::ViewStateOp>& a_state)
	{
		for (const auto& op : a_state) {
			m_retainedState.Set(op.mod, op.key, op.value, false);
			PublishModState(op.mod, op.key, op.value);
		}
	}

	void Runtime::ProcessBackendMessages(const API::Papyrus::PendingBatch& a_papyrus)
	{
		if (m_bridge) {
			// Cancel old requests before processing replies from the previous game session.
			if (a_papyrus.sessionReset) {
				m_bridge->RejectAll("game-load", "the request was canceled by a game load");
			}
			for (const auto& event : a_papyrus.events) {
				const auto targets = InstantiatedViewsOfMod(event.mod);
				if (!targets.empty()) {
					m_bridge->Emit(targets, std::format("{}.{}", event.mod, event.name),
						nlohmann::json{ { "args", event.args } });
				}
			}
			for (const auto& reply : a_papyrus.replies) {
				if (reply.rejected) {
					m_bridge->RejectTo(reply.token, reply.code, reply.message);
				} else {
					m_bridge->RespondTo(reply.token, reply.value);
				}
			}
		}
		API::BridgeApi::Get().PumpRuntimeCallbacks();
	}

	void Runtime::ReconcileFrameState()
	{
		// Open/Close only queue UI messages, so one menuArray scan holds for this whole pass.
		const bool focusMenuInEngine = FocusMenu::IsOpenInEngine();
		const bool surface = static_cast<bool>(SurfaceService::Get().InputRenderer());
		const bool fullscreen = SurfaceService::Get().FullscreenActive();
		const bool capture = m_presentation.DesiredCapture() || surface;
		m_inputCapture.ReconcileFocusMenu(capture, focusMenuInEngine, m_nowSeconds);
		m_inputCapture.ReconcileBrowserFocus(InputRenderer().get(), m_visible.load() || surface, m_presentation.ActiveMenu().has_value() || surface, focusMenuInEngine);
		m_inputCapture.ReconcileControlLayer(capture, IsInputCaptured());
		// A fullscreen surface behaves like a pausing menu.
		SimPause::Apply(m_presentation.DesiredPause() || fullscreen);
		FreeCursor::Apply(m_presentation.DesiredCapture() || fullscreen);
		if (m_compositor) m_compositor->SetFullscreenSource(SurfaceService::Get().FullscreenFrames());
	}

	void Runtime::CommitPresentation()
	{
		// Legacy ready callbacks may publish state mid-batch; requests they enqueue belong to the next batch.
		ApplyNativeState(API::BridgeApi::Get().TakePendingState());
		DrivePendingOpen();
		ApplyViewPresentationPolicy();
	}

	void Runtime::ProcessRendererNotifications()
	{
		Compat::V1::Pump();
        if (m_bridge) {
            std::vector<std::string> legacyViews;
            for (const auto& manifest : m_views.All())
                if (manifest.legacy && m_presentation.IsInstantiated(manifest.id)) legacyViews.push_back(manifest.id);
            Compat::V1::Papyrus::Pump(*m_bridge, legacyViews, !IsInputCaptured() && !MenuEventSink::TransitionOpen());
        }

		// Install pending native endpoints before incoming pages can call them.
		API::BridgeApi::Get().PumpRuntimeCallbacks();
		if (m_renderer) m_renderer->DrainNotifications();
		SurfaceService::Get().Update(m_dataLoaded);
	}

	void Runtime::ProcessRendererFrame()
	{
		if (!m_renderer) return;
		DriveDevTools();
		PumpDevViewReload();
		if (const auto clientSize = OverlayInputHook::GameWindowClientSize()) {
			OnOutputResized(clientSize->width, clientSize->height);
		}
		if (const auto move = m_pointerInput.TakeMouseMove()) {
			m_renderer->InjectMouseMove(move->x, move->y);
		}
		if (m_compositor) {
			m_compositor->Update(); // retire reads and adopt rings even while hidden
		}
		m_renderer->Update(); // starts a newly demanded host in this update
		UpdatePendingOpen();
		UpdateViewReveal();
	}

	void Runtime::Update()
	{
		if (!m_osfSettings.Available()) return;
		// Other window hooks can swallow focus messages. Repair the browser gate before Back or input is routed.
		OnGameWindowActivation(OverlayInputHook::IsGameWindowActive());
		const auto now = std::chrono::steady_clock::now();
		m_nowSeconds = std::chrono::duration<double>(now.time_since_epoch()).count();
		ProcessLifecycleWork();
		// Drain browser handlers before taking this update's single request batch.
		ProcessRendererNotifications();
		auto bridgeBatch = API::BridgeApi::Get().TakePendingBatch();
		auto papyrusBatch = API::Papyrus::TakePendingBatch();
		// State before legacy ready callbacks can publish newer values; messages after view registration.
		ProcessBackendState(papyrusBatch, bridgeBatch.state);
		DrainViewRegistrations(bridgeBatch.viewRegistrations);
		ProcessBackendMessages(papyrusBatch);
		ApplyPresentationRequests(bridgeBatch.presentation);
		// After notifications drain, before deadlines. Recovery only prepares hidden views.
		if (m_renderer) {
			DriveBrowserHostRecovery();
			DriveRecovery();
		}
		UpdateSurfaceInteraction();
		CommitPresentation();
		RouteGamepadInput();
		DrainSurfacePointer();
		ProcessRendererFrame();
        const bool overlayAvailable = m_browserHostRecovery.IsAvailable() && std::ranges::any_of(m_views.All(),
            [this](const auto& view) { return m_presentation.IsInstantiated(view.id); });
        API::BridgeApi::Get().SetBridgeAvailability(overlayAvailable || SurfaceService::Get().HasAvailableViews());
		// Process queued replies before checking timeouts.
		if (m_bridge) m_bridge->Tick(now);
		m_relativePointer.Drain();
		if (!m_lastShownView.empty()) {
			API::BridgeApi::Get().DispatchViewLifecycle(
				m_lastShownView, API::ViewLifecyclePhase::kFrame);
		}
	}
}
