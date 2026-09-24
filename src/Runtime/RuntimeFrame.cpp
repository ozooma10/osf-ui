#include "Runtime/Runtime.h"

#include "API/PapyrusApi.h"
#include "Input/FreeCursor.h"
#include "Input/OverlayInputHook.h"

namespace OSFUI
{
	void Runtime::ProcessLifecycleWork()
	{
		if (_dataLoadedInit.Take()) InitializeDataLoadedState();
		DriveBrowserHostRecovery();
	}

	void Runtime::ProcessBackendQueues(API::Papyrus::PendingBatch a_papyrus,
		std::vector<API::BridgeApi::ViewStateOp> a_bridgeState)
	{
		// Retain owner state before the first WebView exists; its greeting will replay it.
		if (a_papyrus.sessionReset) _retainedState.ClearSessionScoped();
		for (const auto& state : a_papyrus.states) {
			_retainedState.Set(state.mod, state.key, state.value, true);
			PublishModState(state.mod, state.key, state.value);
		}
		for (auto& op : a_bridgeState) {
			_retainedState.Set(op.mod, op.key, op.value, false);
			PublishModState(op.mod, op.key, op.value);
		}
		if (_bridge) {
			for (const auto& event : a_papyrus.events) {
				const auto targets = InstantiatedViewsOfMod(event.mod);
				if (!targets.empty()) {
					_bridge->Emit(targets, std::format("{}.{}", event.mod, event.name),
						nlohmann::json{ { "args", event.args } });
				}
			}
			for (const auto& reply : a_papyrus.replies) {
				if (reply.rejected) _bridge->RejectTo(reply.deferToken, reply.code, reply.message);
				else _bridge->RespondTo(reply.deferToken, reply.value);
			}
			_bridge->Tick();
		}
		API::BridgeApi::Get().PumpMainThread();
	}

	void Runtime::ReconcileFrameState(double a_deltaSeconds)
	{
		ReconcileFocusMenu();
		ReconcileInputFocus();
		ReconcileControlLayer();
		ReconcileSimPause();
		FreeCursor::Apply(_presentation.DesiredCapture());
		RouteGamepadInput(a_deltaSeconds);
	}

	void Runtime::ProcessRendererFrame(double a_deltaSeconds)
	{
		if (!_renderer) return;
		DriveRecovery();
		DriveDevTools();
		PumpDevViewReload();
		if (const auto clientSize = OverlayInputHook::GameWindowClientSize()) {
			_gameClientSizeObserved.store(true, std::memory_order_release);
			OnOutputResized(clientSize->width, clientSize->height);
		} else if (!_gameClientSizeObserved.load(std::memory_order_acquire) && _compositor) {
			if (const auto targetSize = _compositor->GetObservedOutputSize()) {
				OnOutputResized(targetSize->width, targetSize->height);
			}
		}
		if (const auto packed = _pendingMouseMove.exchange(kNoPendingMouseMove);
			packed != kNoPendingMouseMove) {
			_renderer->InjectMouseMove(static_cast<int>(packed >> 32),
				static_cast<int>(packed & 0xFFFF'FFFFull));
		}
		if (_compositor) {
			_compositor->Update(); // retire reads and adopt rings even while hidden
		}
		_renderer->Update(a_deltaSeconds);
		DrivePendingOpen();
		UpdateViewReveal();
	}

	void Runtime::Tick(double a_deltaSeconds)
	{
		if (!_initialized) return;
		// Flush even while inert or before any renderer exists (including startup failures).
		_osfSettings.RetryDiagnostics();
		if (!_osfSettings.Available()) return;
		++_mainTickSerial;
		_uptime += a_deltaSeconds;
		ProcessLifecycleWork();
		auto bridgeBatch = API::BridgeApi::Get().TakePendingBatch();
		DrainViewRegistrations(std::move(bridgeBatch.viewRegistrations));
		const auto presentationWork = TakePresentationRequests(std::move(bridgeBatch.presentation));
		auto papyrusBatch = API::Papyrus::TakePendingBatch();
		ProcessBackendQueues(std::move(papyrusBatch), std::move(bridgeBatch.state));
		ApplyPresentationRequests(presentationWork);
		ReconcileFrameState(a_deltaSeconds);
		ProcessRendererFrame(a_deltaSeconds);
		DrainRelativePointerCapture();
		if (!_lastShownView.empty()) {
			API::BridgeApi::Get().DispatchViewLifecycle(
				_lastShownView, API::ViewLifecyclePhase::kFrame);
		}
	}
}
