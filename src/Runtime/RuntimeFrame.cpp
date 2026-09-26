#include "Runtime/Runtime.h"

#include "API/PapyrusApi.h"
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
		// The only menu admission gate; evaluated once per update.
		const bool suspend = !m_inputCapture.MenuEventsAvailable() || MenuEventSink::TransitionOpen() || !m_browserHostRecovery.IsAvailable();
		if (m_presentation.SetSuspended(suspend) && suspend) {
			m_viewOpens.CancelMenu();
		}
	}

	void Runtime::ProcessBackendState(const API::Papyrus::PendingBatch& a_papyrus, const std::vector<API::BridgeApi::ViewStateOp>& a_bridgeState)
	{
		// Retain owner state before the first WebView exists; its greeting will replay it.
		if (a_papyrus.sessionReset) {
			m_retainedState.ClearSessionScoped();
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
			// Reject before this batch's replies so an old-session answer queued after the reset finds its token gone.
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
		m_inputCapture.ReconcileFocusMenu(m_presentation.DesiredCapture(), focusMenuInEngine, m_nowSeconds);
		m_inputCapture.ReconcileBrowserFocus(m_renderer.get(), m_visible.load(), m_presentation.ActiveMenu().has_value(), focusMenuInEngine);
		m_inputCapture.ReconcileControlLayer(m_presentation.DesiredCapture(), IsInputCaptured());
		SimPause::Apply(m_presentation.DesiredPause());
		FreeCursor::Apply(m_presentation.DesiredCapture());
	}

	void Runtime::CommitPresentation()
	{
		// Ready callbacks may publish state while this batch is prepared.
		// Consume only that state: callback-enqueued requests belong to the next batch.
		ApplyNativeState(API::BridgeApi::Get().TakePendingState());
		DrivePendingOpen();
		ApplyViewPresentationPolicy();
	}

	void Runtime::ProcessRendererNotifications()
	{
		// Install pending native endpoints before incoming pages can call them.
		API::BridgeApi::Get().PumpRuntimeCallbacks();
		if (m_renderer) m_renderer->DrainNotifications();
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
		UpdateViewReveal();
	}

	void Runtime::Update()
	{
		if (!m_initialized || !m_osfSettings.Available()) return;
		const auto now = std::chrono::steady_clock::now();
		m_nowSeconds = std::chrono::duration<double>(now.time_since_epoch()).count();
		ProcessLifecycleWork();
		// Browser handlers enqueue alongside native calls and hotkeys. Drain them
		// before taking the single finite request batch for this update.
		ProcessRendererNotifications();
		auto bridgeBatch = API::BridgeApi::Get().TakePendingBatch();
		auto papyrusBatch = API::Papyrus::TakePendingBatch();
		// Apply captured state before ready callbacks can publish newer values.
		// Messages still follow view registration.
		ProcessBackendState(papyrusBatch, bridgeBatch.state);
		DrainViewRegistrations(bridgeBatch.viewRegistrations);
		ProcessBackendMessages(papyrusBatch);
		ApplyPresentationRequests(bridgeBatch.presentation);
		// Observe queued host responses before deadlines; restart only after its
		// notification callbacks have returned. Recovery only prepares hidden views.
		if (m_renderer) {
			DriveBrowserHostRecovery();
			DriveRecovery();
		}
		CommitPresentation();
		RouteGamepadInput();
		ProcessRendererFrame();
		// Native, Papyrus and browser queues get to settle requests before timeout checks.
		if (m_bridge) m_bridge->Tick(now);
		m_relativePointer.Drain();
		if (!m_lastShownView.empty()) {
			API::BridgeApi::Get().DispatchViewLifecycle(
				m_lastShownView, API::ViewLifecyclePhase::kFrame);
		}
	}
}
