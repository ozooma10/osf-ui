#pragma once

#include <atomic>
#include <chrono>
#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include "API/BridgeApi.h"
#include "API/PapyrusApi.h"
#include "Bridge/MessageBridge.h"
#include "Bridge/RetainedStateStore.h"
#include "Composite/D3D12Compositor.h"
#include "Dependency/OSFSettingsClient.h"
#include "Input/GamepadSession.h"
#include "Input/InputCaptureController.h"
#include "Input/PointerInputState.h"
#include "Input/RelativePointerSession.h"
#include "Input/ViewInputGrants.h"
#include "Input/XInputPoller.h"
#include "Render/BrowserHostRecovery.h"
#include "Render/WebView2HostWebRenderer.h"
#include "Views/Dev/DevViewReloadWorker.h"
#include "Views/ViewLoadTracker.h"
#include "Views/ViewManager.h"
#include "Views/ViewOpenCoordinator.h"
#include "Views/ViewPresentationController.h"
#include "Views/ViewRecoveryTracker.h"
#include "Views/ViewRevealGate.h"
#include "Views/ViewRequestQueue.h"

namespace OSFUI
{
	class Runtime
	{
	public:
		static Runtime& Get();

		// SFSE lifecycle and main-thread frame entry points.
		bool Initialize();
		void OnPostPostLoad();
		void OnDataLoaded();
		void OnPostDataLoaded();
		void Tick(double a_deltaSeconds);

		// Published state, safe to query from the window-message thread.
		bool IsVisible() const;
		bool IsInputCaptured() const;

		// Queued requests; applied on the main-thread tick.
		void EnqueuePresentationRequest(ViewPresentationRequest a_req);
		void EnqueueOpenView(std::string a_viewId);
		// Browser transport threads only enqueue this ownership edge; Runtime applies
		// it beside presentation work so every native callback stays on the game main thread.
		void EnqueueRelativePointerCapture(std::string a_viewId, bool a_active);

		// WndProc input entry points (window-message thread).
		bool OnGameWindowKeyboard(const osfui::wv2::msg::Keyboard& a_key);
		void OnGameWindowText(const osfui::wv2::msg::TextInput& a_text);
		void OnGameWindowActivation(bool a_active);
		void OnGameWindowMouseAbsolute(int a_clientX, int a_clientY, int a_clientW, int a_clientH);
		// Accumulate one packet for the active relative-pointer owner.
		bool OnGameWindowMouseRelative(int a_dx, int a_dy);
		void OnGameWindowMouseButton(int a_button, bool a_down);
		void OnGameWindowMouseWheel(int a_wheelDelta);

	private:
		Runtime() = default;

		// Startup and lazy initialization.
		void LoadStartupContent();
		void InitializeDataLoadedState();
		bool EnsureWebRuntime();
		bool InitializeRenderer();
		void WireRendererLifecycleCallbacks();
		bool InitializeCompositor();
		void InitializeBridge();
		void InitializeStartupViews();

		// Frame stages (RuntimeFrame.cpp).
		void ProcessLifecycleWork();
		void ProcessBackendQueues(API::Papyrus::PendingBatch a_papyrus,
			std::vector<API::BridgeApi::ViewStateOp> a_bridgeState);
		void ReconcileFrameState(double a_deltaSeconds);
		void ProcessRendererFrame(double a_deltaSeconds);

		// View requests, presentation and output geometry.
		void ApplyPresentationRequests(
			const std::vector<ViewRequestQueue::Operation>& a_local,
			const std::vector<API::BridgeApi::ViewPresentationRequest>& a_plugin);
		bool BeginViewOpen(std::string_view a_id, std::string_view a_reason = "on demand",
			std::optional<std::chrono::steady_clock::time_point> a_requestedAt = std::nullopt);
		void DrivePendingOpen();
		ViewOpenCoordinator::Readiness ViewOpenReadiness(std::string_view a_id) const;
		void DrainViewRegistrations(std::vector<std::string> a_ids);
		void ApplyViewPresentationPolicy();
		void OnOutputResized(std::uint32_t a_width, std::uint32_t a_height);
		void UpdateViewReveal();

		// View loading and per-view recovery (Views/RuntimeViews.cpp).
		bool InstantiateView(const ViewManifest& a_manifest, std::string_view a_reason);
		void NavigateView(const ViewManifest& a_manifest);
		void OnViewLoad(std::string_view a_viewId, bool a_failed, std::string_view a_url,
			std::string_view a_description, int a_errorCode);
		bool HudAutoStartEligible(const ViewManifest& a_manifest) const;
		void TearDownFailedView(const std::string& a_id);
		void DriveRecovery();

		// Browser-host recovery; distinct from a single view failing to load.
		void OnRendererFailure(const WebView2HostWebRenderer::FailureEvent& a_event);
		// Deferred until the failure callback has returned; recreates every instantiated view.
		void DriveBrowserHostRecovery();
		void RehydrateRendererAfterRestart();

		// Input policy and routing (Input/RuntimeInput.cpp).
		// Runtime applies presentation failure policy around capture reconciliation.
		bool ReconcileInputSuppression();
		void ReconcileFocusMenu();
		// Poll XInput and deliver events to the active document (`ui.gamepad` events).
		void RouteGamepadInput(double a_deltaSeconds);

		// Relative-pointer session; callbacks run on the main-thread tick.
		void ApplyRelativePointerRequests(const std::vector<ViewRequestQueue::RelativePointerRequest>& a_requests);

		// Bridge endpoints, retained state and protocol lifecycle (Bridge/RuntimeBridge.cpp).
		void RegisterPlatformEndpoints(MessageBridge& a_bridge);
		void BroadcastViewsData();
		std::unordered_set<std::string> InstantiatedViewsOfMod(std::string_view a_mod) const;
		void PublishModState(std::string_view a_mod, std::string_view a_key, const nlohmann::json& a_value);
		void PublishViewsState(std::string_view a_viewId = {});
		void OnViewGreeted(std::string_view a_viewId);
		void OnProtocolFault(std::string_view a_viewId, std::string_view a_code,
			std::string_view a_message, const nlohmann::json& a_detail, bool a_viewFault);

		// Developer tools and view diagnostics.
		void DriveDevTools();
		void PumpDevViewReload();
		nlohmann::json BuildViewsData() const;

		// Ownership: ordinary mutable fields below belong to the main-thread tick.
		// Startup initializes paths/catalog/settings before input hooks are installed;
		// _initialized, _developerMode and the renderer pointer then stay stable.
		// Renderer load/failure callbacks run when its queues drain on that tick.

		// Owned services. Keep their construction/destruction order explicit.
		ViewManager _views;
		std::unique_ptr<WebView2HostWebRenderer> _renderer;
		std::unique_ptr<D3D12Compositor> _compositor;
		std::unique_ptr<MessageBridge> _bridge;
		OSFSettingsClient _osfSettings;
		// Main owns the worker; its synchronized interface owns cross-thread jobs.
		std::unique_ptr<DevViewReloadWorker> _devViewReload;

		// Startup and frame lifecycle.
		bool _initialized{ false };
		bool _postPostLoadAttempted{ false };
		bool _webRuntimeInitializing{ false };
		bool _webRuntimeReady{ false };
		bool _developerMode{ false };       // startup-latched; changes apply next launch
		bool _highRefreshCapture{ false };  // startup-latched explicit 240 Hz opt-in
		std::uint64_t _mainTickSerial{ 0 };
		double _uptime{ 0.0 };
		// SFSE lifecycle producer -> main-thread consumer.
		std::atomic_bool _dataLoadedInitPending{ false };
		std::atomic_bool _postDataLoadedReady{ false };

		// View lifecycle and presentation.
		ViewPresentationController _presentation;
		ViewOpenCoordinator _viewOpens;
		ViewLoadTracker m_viewLoads;
		ViewRevealGate m_viewReveal;
		ViewRecoveryTracker m_viewRecovery;
		std::string _lastShownView;
		std::atomic_bool m_visible{ false };  // main -> WndProc/frame-event producer
		// Mutex-protected producer queue; only the main thread takes/applies batches.
		ViewRequestQueue m_viewRequests;

		// Browser-host recovery.
		BrowserHostRecovery _browserHostRecovery;
		bool _rendererFailed{ false };          // opens fail closed until recovery completes
		bool _rendererFailureLatched{ false };  // first failure per helper wins

		// Input components own their state and cross-thread publication boundaries.
		InputCaptureController _inputCapture;
		PointerInputState _pointerInput;
		RelativePointerSession _relativePointer;
		ViewInputGrants m_viewInputGrants;
		XInputPoller m_gamepadSource;
		GamepadSession m_gamepadSession;

		// Bridge state and protocol diagnostics.
		RetainedStateStore _retainedState;
		std::unordered_map<std::string, std::uint32_t> _viewProtocolFaultCounts;
		std::string _lastViewsData;

		// Developer tools request (WndProc -> main).
		std::atomic_bool _devToolsRequested{ false };
	};
}
