#include "Runtime/Runtime.h"

#include "Core/Paths.h"
#include "World/WorldTexture.h"
#include "Input/OverlayInputHook.h"
#include "Input/MenuEventSink.h"
#include "Platform/WindowsPlatform.h"
#if defined(OSFUI_TEST_HARNESS)
#include "WorldSurfaceObservations.h"
#endif

namespace OSFUI
{
	void Runtime::CancelWorldInteraction()
	{
		_worldInteractionCancel.store(true, std::memory_order_release);
	}

	void Runtime::FinishWorldInteraction(const char* a_reason)
	{
		if (_pendingWorldInteraction) {
			const auto request = std::move(*_pendingWorldInteraction);
			_pendingWorldInteraction.reset();
			request.callback(request.token, request.view.c_str(), API::Views::InteractionPhase::kRejected, a_reason, request.context);
		}
		if (!_worldInteraction) return;
		const auto owner = std::move(*_worldInteraction);
		_worldInteraction.reset();
		if (auto* renderer = _worldInputRenderer.exchange(nullptr, std::memory_order_acq_rel)) {
			renderer->SetNativeFocus(false);
			renderer->SetAcceleratorKeys(kInvalidScanCode, false, false, kInvalidScanCode);
		}
		_worldNativeFocus = false;
		_worldInteractionCancel.store(false, std::memory_order_release);
		if (_bridge) _bridge->Emit(owner.view, "ui.interaction", nlohmann::json{ { "active", false }, { "reason", a_reason } });
		(void)m_gamepadSession.End();
		m_gamepadSource.Reset();
		OverlayInputHook::RequestStateRefresh();
		REX::INFO("WorldInteraction: '{}' ended ({})", owner.view, a_reason);
		owner.callback(owner.token, owner.view.c_str(), API::Views::InteractionPhase::kEnded, a_reason, owner.context);
	}

	void Runtime::ApplyWorldInteractionRequests(const std::vector<API::BridgeApi::InteractionRequest>& a_requests)
	{
		if (_worldInteractionCancel.exchange(false)) FinishWorldInteraction("focus-or-menu");
		for (const auto& request : a_requests) {
			if (!request.callback) {
				if ((_worldInteraction && _worldInteraction->token == request.token) ||
					(_pendingWorldInteraction && _pendingWorldInteraction->token == request.token)) FinishWorldInteraction("requested");
				continue;
			}
			if (_worldInteraction || _pendingWorldInteraction) {
				request.callback(request.token, request.view.c_str(), API::Views::InteractionPhase::kRejected, "busy", request.context);
			} else {
				_pendingWorldInteraction = request;
				_worldNeutralTick = 0;
			}
		}
		if (_pendingWorldInteraction) {
			const auto request = *_pendingWorldInteraction;
			const auto world = std::ranges::find_if(_worldViews, [&](const auto& view) { return view.manifest.id == request.view; });
			const auto stats = WorldTexture::Snapshot();
			const auto surface = std::ranges::find_if(stats, [&](const auto& view) { return view.id == request.view; });
			const bool available = std::chrono::steady_clock::now() - request.requestedAt < std::chrono::seconds(5) &&
				!_worldInteraction && !_presentation.ActiveMenu() && !_pendingViewOpen && Platform::GameIsForeground() &&
				!MenuEventSink::ConsoleOpen() && world != _worldViews.end() && world->renderer && !world->failed &&
				m_viewLoads.GetState(request.view) == ViewLoadState::Finished &&
				surface != stats.end() && surface->lastCompletedFrame != 0 && !surface->gpuFailed && EnsureCaptureIntegration();
			if (!available) {
				FinishWorldInteraction("unavailable");
				return;
			}
			// Let Starfield observe the activation key's release before moving native
			// focus; otherwise its next press can be mistaken for a held-key repeat.
			if (Platform::AnyKeyDown() || m_gamepadSource.Poll().buttons != 0) {
				_worldNeutralTick = 0;
				return;
			}
			if (!_worldNeutralTick) _worldNeutralTick = _mainTickSerial;
			if (_mainTickSerial - _worldNeutralTick < 2) return;
			if (!_osfSettings.AcquireInputSuppression()) {
				FinishWorldInteraction("input-unavailable");
				return;
			}
			_pendingWorldInteraction.reset();
			_worldInteraction = request;
			world->renderer->SetInputTargetView(request.view);
			world->renderer->SetAcceleratorKeys(kInvalidScanCode, true, false, kInvalidScanCode);
			_worldInputRenderer.store(world->renderer.get(), std::memory_order_release);
			(void)m_gamepadSession.End();
			m_gamepadSource.Reset();
			OverlayInputHook::RequestStateRefresh();
			if (_bridge) _bridge->Emit(request.view, "ui.interaction", nlohmann::json{ { "active", true } });
			REX::INFO("WorldInteraction: '{}' started", request.view);
			request.callback(request.token, request.view.c_str(), API::Views::InteractionPhase::kStarted, "requested", request.context);
		}
	}

	void Runtime::ConfigureWorldViews()
	{
		std::vector<WorldTexture::SurfaceDesc> surfaces;
		for (const auto& manifest : _views.All()) {
			if (manifest.kind != ViewKind::World || (manifest.debugOnly && !_developerMode)) continue;
			if (surfaces.size() == WorldTexture::kMaxSurfaces ||
				std::ranges::any_of(surfaces, [&](const auto& surface) {
					return surface.placeholderSize == manifest.placeholderSize;
				})) {
				REX::ERROR("WorldSurface: '{}' has a duplicate placeholder size or exceeds the surface limit", manifest.id);
				continue;
			}
			surfaces.push_back({ manifest.id, manifest.placeholderSize, manifest.width, manifest.height });
			_worldViews.push_back({ .manifest = manifest });
		}
		if (surfaces.empty()) return;
		if (!WorldTexture::Configure(surfaces)) {
			_worldViews.clear();
			return;
		}
		InitializeBridge();
		REX::INFO("WorldSurface: configured {} material-backed view(s); browser startup waits for a matching texture", surfaces.size());
	}

	bool Runtime::IsWorldViewInstantiated(std::string_view a_id) const
	{
		return std::ranges::any_of(_worldViews, [&](const auto& world) {
			return world.manifest.id == a_id && world.renderer && !world.failed;
		});
	}

	void Runtime::SendToWorldView(std::string_view a_id, std::string_view a_json)
	{
		for (auto& world : _worldViews) {
			if (world.manifest.id == a_id && world.renderer && !world.failed) {
				world.renderer->SendMessageToWeb(a_id, a_json);
				return;
			}
		}
	}

	void Runtime::TickWorldViews(double a_deltaSeconds)
	{
		if (_worldViews.empty() || !WorldTexture::Install()) return;
		const auto stats = WorldTexture::Snapshot();
		const auto detachFailedView = [this](WorldView& instance) {
			if (_worldInteraction && _worldInteraction->view == instance.manifest.id) FinishWorldInteraction("browser-failed");
			instance.failed = true;
			m_viewLoads.FinishLoad(instance.manifest.id, true);
			API::BridgeApi::Get().SetViewInstantiated(instance.manifest.id, false);
			_bridge->OnViewDestroyed(instance.manifest.id);
			m_viewInputGrants.ResetPage(instance.manifest.id);
			BroadcastViewsData();
		};
		bool active = false;
		for (std::size_t i = 0; i < _worldViews.size(); ++i) {
			auto& world = _worldViews[i];
			const auto& manifest = world.manifest;
			if (stats[i].gpuFailed && _worldInteraction && _worldInteraction->view == manifest.id) {
				FinishWorldInteraction("render-failed");
			}
			if (!world.renderer && !world.failed && stats[i].boundDescriptors != 0) {
				world.renderer = std::make_unique<WebView2HostWebRenderer>();
				if (!world.renderer->Initialize({
					.width = manifest.width, .height = manifest.height,
					.devMode = _developerMode, .highRefreshCapture = false,
					.dataDir = Paths::DataDir(), .instanceName = std::format("world-{}", i)
				})) {
					world.failed = true;
					world.renderer.reset();
					_osfSettings.ReportFailure("world." + manifest.id, "world.host-init", "World view browser initialization failed");
					continue;
				}
				world.renderer->SetSharedRingHandler([i](const SharedRingDesc& desc) { WorldTexture::SetSharedRing(i, desc); });
				world.renderer->SetNativeAcceleratorHandler([this](std::uint32_t vk, std::uint32_t scan, bool down) {
					return OnNativeAcceleratorKey(vk, scan, down);
				});
				world.renderer->SetWebMessageHandler([this, i](std::string_view id, std::string_view json) {
					const auto& instance = _worldViews[i];
					if (_bridge && !instance.failed && id == instance.manifest.id) {
						_bridge->HandleWebMessage(id, json);
					}
				});
				world.renderer->SetLoadHandler([this, i, detachFailedView](const WebView2HostWebRenderer::LoadEvent& event) {
					auto& instance = _worldViews[i];
					// Once a host/document fails, only the explicit restart below
					// admits new events; a queued old success must not revive its gate.
					if (instance.failed || event.viewId != instance.manifest.id) return;
					m_viewLoads.FinishLoad(instance.manifest.id, event.failed);
					if (event.failed) {
						detachFailedView(instance);
						instance.recovery.OnRetryableFailure(_uptime);
						_osfSettings.ReportFailure("world." + instance.manifest.id, "world.load-failed",
							"World view page failed to load; automatic recovery is bounded");
						REX::ERROR("WorldSurface: '{}' load failed: {}", instance.manifest.id, event.description);
					} else {
						instance.failed = false;
						instance.recovery.Reset();
						API::BridgeApi::Get().SetBridgeAvailability(_bridge.get());
						API::BridgeApi::Get().SetViewInstantiated(instance.manifest.id, true);
						_osfSettings.ClearFailure("world." + instance.manifest.id);
						BroadcastViewsData();
						REX::INFO("WorldSurface: '{}' loaded", instance.manifest.id);
					}
				});
				world.renderer->SetFailureHandler([this, i, detachFailedView](const WebView2HostWebRenderer::FailureEvent& event) {
					auto& instance = _worldViews[i];
					if (instance.failed) return;
					detachFailedView(instance);
					instance.recovery.OnRetryableFailure(_uptime);
					_osfSettings.ReportFailure("world." + instance.manifest.id, "world.host-failed",
						"World view browser stopped; the screen retains its last completed image");
					REX::ERROR("WorldSurface: '{}' host failed at '{}': {}", instance.manifest.id, event.stage, event.description);
				});
				m_viewLoads.BeginLoad(manifest.id);
				_bridge->OnViewCreated(manifest.id);
				API::BridgeApi::Get().SetBridgeAvailability(_bridge.get());
				API::BridgeApi::Get().SetViewInstantiated(manifest.id, true);
				world.renderer->CreateOrNavigateView(manifest);
				world.renderer->SetViewHidden(manifest.id, false);
				world.renderer->SetPointerInputEnabled(false);
				BroadcastViewsData();
				REX::INFO("WorldSurface: '{}' starting dedicated {}x{} browser", manifest.id, manifest.width, manifest.height);
			}
			if (!world.renderer) continue;
			active = true;
			if (world.recovery.ExpireResponseWait(_uptime)) {
				detachFailedView(world);
				_osfSettings.ReportFailure("world." + manifest.id, "world.recovery-timeout",
					"World view browser did not respond during recovery");
			}
			if (world.recovery.BeginDueAttempt(_uptime)) {
				REX::INFO("WorldSurface: '{}' restarting browser (attempt {})", manifest.id, world.recovery.Attempts());
				if (world.renderer->RestartAfterFailure()) {
					world.failed = false;
					_bridge->OnViewDestroyed(manifest.id);
					_bridge->OnViewCreated(manifest.id);
					API::BridgeApi::Get().SetBridgeAvailability(_bridge.get());
					API::BridgeApi::Get().SetViewInstantiated(manifest.id, true);
					m_viewLoads.BeginLoad(manifest.id);
					world.renderer->CreateOrNavigateView(manifest);
					world.renderer->SetViewHidden(manifest.id, false);
					world.renderer->SetPointerInputEnabled(false);
					BroadcastViewsData();
				} else world.recovery.OnAttemptSetupFailed(_uptime);
			}
			world.renderer->Update(a_deltaSeconds);
			if (const auto frame = world.renderer->TakeLatestFrame()) WorldTexture::Submit(i, *frame);
		}
		_worldViewsActive.store(active, std::memory_order_release);
#if defined(OSFUI_TEST_HARNESS)
		if (_uptime >= _worldSnapshotAt) {
			_worldSnapshotAt = _uptime + 1.0;
			Testing::LogWorldSurfaceObservations(stats);
		}
#endif
	}
}
