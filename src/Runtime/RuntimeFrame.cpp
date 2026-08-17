#include "Runtime/Runtime.h"

#include "API/PapyrusApi.h"
#include "Compat/V1/Papyrus.h"
#include "Input/FreeCursor.h"

namespace OSFUI
{
    void Runtime::ProcessLifecycleWork()
    {
		if(_controlMapInit.Take()) {
			InitializeDataLoadedState();
		}

		if(_uiIntegrationInit.Take()) {
			InitializePostDataLoadIntegration();
		}

		DriveBrowserHostRecovery();
    }

    void Runtime::ProcessControlMapUpdates()
    {
		if(_keyboardLayoutChanged.exchange(false)) {
			RefreshKeyboardLabels("input language change");

			if(_controlMap.RefreshLabels(false)) {
				SyncLiveControlMapBindings();
				PublishPlatformState("keybindings");
			} else if(_controlMap.Initialized() && !_controlMap.Available()) {
				SyncLiveControlMapBindings();
				SyncLiveControlMapHealth();
				PublishPlatformState("keybindings");
				PublishPlatformState("input-context");
			}
		}

		const auto changes = _controlMap.Pump();

		if(changes.keybindings) {
			SyncLiveControlMapBindings();
			SyncLiveControlMapHealth();
			PublishPlatformState("keybindings");
		}

		if(changes.engineInputContext) {
			SyncLiveControlMapHealth();
			PublishPlatformState("input-context");
		}
	}

    void Runtime::ProcessBackendQueues()
    {
		DrainKeyCapture();
		DrainHotkeys();
		DrainSchemaOps();

		if(_settings) {
			API::Papyrus::DrainSettingsOps(_settings->Store());
		}
		if(_bridge) {
			if(API::Papyrus::TakeSessionReset()) {
				_retainedState.ClearSessionScoped();
				Compat::V1::Papyrus::ClearPendingPushes();
			}

			API::Papyrus::DrainViewState([this](const API::Papyrus::ViewState& a_state) {
				_retainedState.Set(a_state.mod, a_state.key, a_state.value, /*sessionScoped*/ true);
				PublishModState(a_state.mod, a_state.key, a_state.value);
			});

			Compat::V1::Papyrus::DrainPushes([this](const Compat::V1::Papyrus::Push& a_push) {
				const auto targets = InstantiatedViewsOfMod(a_push.mod);
				if (!targets.empty()) _bridge->Emit(targets, "data.push", a_push.payload);
			});

			for(auto& op : API::BridgeApi::Get().TakeViewStateOps()) {
				_retainedState.Set(op.mod, op.key, op.value, /*sessionScoped*/ false);
				PublishModState(op.mod, op.key, op.value);
			}

			API::Papyrus::DrainViewEvents([this](const API::Papyrus::ViewEvent& a_event) {
				const auto targets = InstantiatedViewsOfMod(a_event.mod);
				if (!targets.empty()) {
					_bridge->Emit(targets, std::format("{}.{}", a_event.mod, a_event.name), nlohmann::json{ { "args", a_event.args } });
				}
			});
			API::Papyrus::DrainViewReplies([this](const API::Papyrus::ViewReply& reply) {
				if(reply.rejected) {
					_bridge->RejectTo(reply.deferToken, reply.code, reply.message);
				} else {
					_bridge->RespondTo(reply.deferToken, nlohmann::json{ { "value", reply.value } });
				}
			});

			_bridge->Tick();
		}
		API::BridgeApi::Get().PumpMainThread();
    }

    void Runtime::ProcessSettingsMaintenance()
    {
		if (_settings) {
			_settings->Store().PumpPersistence(_uptime);
			// Schema hot-reload (developer mode): edited
			// settings/*.json files reload live, values preserved; the
			// registry re-broadcast repaints open Mod Settings.
			if (_config.devMode) {
				_settings->PumpSchemaHotReload(_uptime);
				if (_uptime >= _nextLocalizationScan) {
					_nextLocalizationScan = _uptime + SettingsModule::kHotReloadScanSeconds;
					if (_localization.ReloadIfChanged()) {
						RefreshLocalizedData();
					}
				}
			}
		}
    }

    void Runtime::ReconcileFrameState(double a_deltaSeconds)
    {
		ReconcileFocusMenu();
		ReconcileControlLayer();
		ReconcileSimPause();
		FreeCursor::Apply(_presentation.DesiredCapture());
		RouteGamepadInput(a_deltaSeconds);
    }

    void Runtime::ProcessRendererFrame(double a_deltaSeconds)
    {
		if (!_renderer) {
			return;
		}

		DriveRecovery();
		DriveViewLifecycle();
		DriveDevTools();
		PumpDevViewReload();

		if (const auto packed = _pendingMouseMove.exchange(kNoPendingMouseMove);
			packed != kNoPendingMouseMove) {
			_renderer->InjectMouseMove(static_cast<int>(packed >> 32), static_cast<int>(packed & 0xFFFF'FFFFull));
			++_mouseMoveSends;
		}
		if (_config.devMode && _uptime >= _nextMouseStatsLog) {
			_nextMouseStatsLog = _uptime + 5.0;
			const auto packets = _mouseMovePackets.exchange(0, std::memory_order_relaxed);
			if (packets != 0 || _mouseMoveSends != 0) {
				REX::DEBUG("Runtime: coalesced {} mouse-move packets into {} sends over ~5s", packets, _mouseMoveSends);
				_mouseMoveSends = 0;
			}
		}
		{
			_renderer->SetAcceleratorKeys(_toggleKey.load(std::memory_order_acquire), IsInputCaptured(), _captureArmed.load(), _captureUpScan.load());
			_renderer->Update(a_deltaSeconds);
			DrivePendingOpen();
			SubmitFrameIfVisible();
		}
		_runtimeHealth.Pump();
    }

    void Runtime::Tick(double a_deltaSeconds)
	{
		if (!_initialized) {
			return;
		}
		_uptime += a_deltaSeconds;

		ProcessLifecycleWork();
		DrainViewRegistrations();
		const auto presentationWork = TakePresentationRequests();
		PreparePresentationRequests(presentationWork);

		ProcessControlMapUpdates();
		ProcessBackendQueues();
		ApplyPresentationRequests(presentationWork);

		ProcessSettingsMaintenance();
		ReconcileFrameState(a_deltaSeconds);
		ProcessRendererFrame(a_deltaSeconds);
	}
}
