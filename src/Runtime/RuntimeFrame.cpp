#include "Runtime/Runtime.h"

#include "API/PapyrusApi.h"
#include "Input/FreeCursor.h"
#include "Input/SimPause.h"
#include "Input/MenuEventSink.h"
#include "Input/OverlayInputHook.h"

namespace OSFUI
{
	void Runtime::ProcessLifecycleWork()
	{
		if (m_dataLoadedInitPending.exchange(false, std::memory_order_acq_rel)) InitializeDataLoadedState();
		m_inputCapture.ObserveLifecycle(m_postDataLoadedReady.load(std::memory_order_acquire));
		if (MenuEventSink::TransitionOpen()) m_viewOpens.SuspendMenus();
		if (m_presentation.SetSuspended(!m_inputCapture.MenuEventsAvailable() || MenuEventSink::TransitionOpen() || m_rendererFailed)) {
			ApplyViewPresentationPolicy();
		}
	}

	void Runtime::ProcessBackendQueues(API::Papyrus::PendingBatch a_papyrus,
		std::vector<API::BridgeApi::ViewStateOp> a_bridgeState)
	{
		// Retain owner state before the first WebView exists; its greeting will replay it.
		if (a_papyrus.sessionReset) m_retainedState.ClearSessionScoped();
		for (const auto& state : a_papyrus.states) {
			m_retainedState.Set(state.mod, state.key, state.value, true);
			PublishModState(state.mod, state.key, state.value);
		}
		for (auto& op : a_bridgeState) {
			m_retainedState.Set(op.mod, op.key, op.value, false);
			PublishModState(op.mod, op.key, op.value);
		}
		if (m_bridge) {
			for (const auto& event : a_papyrus.events) {
				const auto targets = InstantiatedViewsOfMod(event.mod);
				if (!targets.empty()) {
					m_bridge->Emit(targets, std::format("{}.{}", event.mod, event.name),
						nlohmann::json{ { "args", event.args } });
				}
			}
			for (const auto& reply : a_papyrus.replies) {
				if (reply.rejected) m_bridge->RejectTo(reply.deferToken, reply.code, reply.message);
				else m_bridge->RespondTo(reply.deferToken, reply.value);
			}
		}
		API::BridgeApi::Get().PumpMainThread();
	}

	void Runtime::ReconcileFrameState()
	{
		ReconcileFocusMenu();
		m_inputCapture.ReconcileBrowserFocus(m_renderer.get(), m_visible.load(), m_presentation.ActiveMenu().has_value());
		m_inputCapture.ReconcileControlLayer(m_presentation.DesiredCapture(), IsInputCaptured());
		SimPause::Apply(m_presentation.DesiredPause());
		FreeCursor::Apply(m_presentation.DesiredCapture());
		RouteGamepadInput();
	}

	void Runtime::ProcessRendererFrame()
	{
		if (!m_renderer) return;
		DriveDevTools();
		PumpDevViewReload();
		if (const auto clientSize = OverlayInputHook::GameWindowClientSize()) {
			m_pointerInput.ObserveGameClientSize();
			OnOutputResized(clientSize->width, clientSize->height);
		} else if (!m_pointerInput.GameClientSizeObserved() && m_compositor) {
			if (const auto targetSize = m_compositor->GetObservedOutputSize()) {
				OnOutputResized(targetSize->width, targetSize->height);
			}
		}
		if (const auto move = m_pointerInput.TakeMouseMove()) {
			m_renderer->InjectMouseMove(move->x, move->y);
		}
		if (m_compositor) {
			m_compositor->Update(); // retire reads and adopt rings even while hidden
		}
		m_renderer->Update();
		// A response queued during a stall must be observed before its deadline expires.
		// Recovery may restart the renderer only after notification callbacks have returned.
		DriveBrowserHostRecovery();
		DriveRecovery();
		DrivePendingOpen();
		UpdateViewReveal();
	}

	void Runtime::Update()
	{
		if (!m_initialized) return;
		if (!m_osfSettings.Available()) return;
		++m_mainTickSerial;
		const auto now = std::chrono::steady_clock::now();
		m_nowSeconds = std::chrono::duration<double>(now.time_since_epoch()).count();
		ProcessLifecycleWork();
		auto bridgeBatch = API::BridgeApi::Get().TakePendingBatch();
		DrainViewRegistrations(std::move(bridgeBatch.viewRegistrations));
		auto localRequests = m_viewRequests.Take();
		auto papyrusBatch = API::Papyrus::TakePendingBatch();
		ProcessBackendQueues(std::move(papyrusBatch), std::move(bridgeBatch.state));
		ApplyPresentationRequests(localRequests.presentation, bridgeBatch.presentation);
		ReconcileFrameState();
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
