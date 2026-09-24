#pragma once

#include <unordered_map>
#include <unordered_set>

#include "API/BridgeApi.h"
#include "API/PapyrusApi.h"
#include "Composite/D3D12Compositor.h"
#include "Dependency/OSFSettingsClient.h"
#include "Input/GamepadSession.h"
#include "Render/WebView2HostWebRenderer.h"
#include "Runtime/AdaptiveViewGeometry.h"
#include "Views/Dev/DevViewReloadWorker.h"
#include "Views/ViewPresentationController.h"
#include "Render/BrowserHostRecovery.h"
#include "Bridge/MessageBridge.h"
#include "Input/ViewInputGrants.h"
#include "Views/ViewManager.h"
#include "Views/ViewLoadTracker.h"
#include "Views/ViewOpenCoordinator.h"
#include "Views/ViewRecoveryTracker.h"
#include "Views/ViewRevealGate.h"
#include "Views/ViewRequestQueue.h"
#include "Bridge/RetainedStateStore.h"

namespace OSFUI
{
	class Runtime
	{
	public:
		static Runtime& Get();

		bool Initialize();
		void OnPostPostLoad();
		void OnDataLoaded();
		void OnPostDataLoaded();

		void Tick(double a_deltaSeconds);

		bool IsVisible() const;

		void EnqueuePresentationRequest(ViewPresentationRequest a_req);

		void EnqueueOpenView(std::string a_viewId);
		// Browser transport threads only enqueue this ownership edge; Runtime applies
		// it beside presentation work so every native callback stays on the game main thread.
		void EnqueueRelativePointerCapture(std::string a_viewId, bool a_active);

		// Published capture state, safe to query from the window-message thread.
		bool IsInputCaptured() const;

		// Called by the WndProc hook on WM_KEYDOWN/WM_KEYUP (window-message thread):
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

		void LoadStartupContent();
		bool EnsureWebRuntime();
		bool EnsureCaptureIntegration();
		bool InitializeRenderer();
		void WireRendererLifecycleCallbacks();
		bool InitializeCompositor();
		void InitializeBridge();
		void InitializeStartupViews();

		void OnOutputResized(std::uint32_t a_width, std::uint32_t a_height);
		void UpdateViewReveal();

		void RegisterPlatformEndpoints(MessageBridge& a_bridge);

		bool InstantiateView(const ViewManifest& a_manifest, std::string_view a_reason);

		void ApplyViewPresentationPolicy();

		// Grant input to the active view after FocusMenu is ready. Main thread only;
		// Windows focus stays in Starfield.
		void ReconcileInputFocus();

		void QueueMouseMove();

		void ApplyPresentationRequests(
			const std::vector<ViewRequestQueue::Operation>& a_local,
			const std::vector<API::BridgeApi::ViewPresentationRequest>& a_plugin);
		void                          ApplyRelativePointerRequests(const std::vector<ViewRequestQueue::RelativePointerRequest>& a_requests);

		bool BeginViewOpen(std::string_view a_id, std::string_view a_reason = "on demand",
			std::optional<std::chrono::steady_clock::time_point> a_requestedAt = std::nullopt);
		void DrivePendingOpen();
		ViewOpenCoordinator::Readiness ViewOpenReadiness(std::string_view a_id) const;

		void DrainViewRegistrations(std::vector<std::string> a_ids);

		// open/close engine focus menu to match active menu capture policy.
		bool ReconcileInputSuppression();
		void ReconcileFocusMenu();

		//Poll XInput and deliver events to active document (`ui.gamepad` events)
		void RouteGamepadInput(double a_deltaSeconds);

		void ReconcileControlLayer();

		void InitializeDataLoadedState();
		
		void ProcessLifecycleWork();
		void ProcessBackendQueues(API::Papyrus::PendingBatch a_papyrus, std::vector<API::BridgeApi::ViewStateOp> a_bridgeState);
		void ReconcileFrameState(double a_deltaSeconds);
		void ProcessRendererFrame(double a_deltaSeconds);


		bool BeginRelativePointerCapture(std::string_view a_viewId);
		void EndRelativePointerCapture(std::string_view a_viewId);
		void CancelRelativePointerCapture(std::string_view a_viewId = {});
		void DrainRelativePointerCapture();
		void FinishRelativePointerCapture(API::RelativePointerPhase a_phase);

		void OnViewLoad(std::string_view a_viewId, bool a_failed, std::string_view a_url, std::string_view a_description, int a_errorCode);

		void NavigateView(const ViewManifest& a_manifest);

		void DriveRecovery();

		bool HudAutoStartEligible(const ViewManifest& a_manifest) const;
		void TearDownFailedView(const std::string& a_id);

		void DriveDevTools();

		void PumpDevViewReload();
		nlohmann::json BuildViewsData() const;

		void OnRendererFailure(const WebView2HostWebRenderer::FailureEvent& a_event);
		// Deferred until the failure callback has returned; recreates every instantiated view.
		void DriveBrowserHostRecovery();
		void RehydrateRendererAfterRestart();

		void BroadcastViewsData();

		std::unordered_set<std::string> InstantiatedViewsOfMod(std::string_view a_mod) const;

		void PublishModState(std::string_view a_mod, std::string_view a_key, const nlohmann::json& a_value);
		void PublishViewsState(std::string_view a_viewId = {});

		void OnViewGreeted(std::string_view a_viewId);
		void OnProtocolFault(std::string_view a_viewId, std::string_view a_code, std::string_view a_message, const nlohmann::json& a_detail, bool a_viewFault);

		// Ownership: ordinary mutable fields below belong to the main-thread tick.
		// Startup initializes paths/catalog/settings before input hooks are installed;
		// _initialized, _developerMode and the renderer pointer then stay stable.
		// Renderer load/failure callbacks run when its queues drain on that tick.
		ViewManager                   _views;
		std::unique_ptr<WebView2HostWebRenderer> _renderer;
		std::unique_ptr<D3D12Compositor> _compositor;
		std::unique_ptr<MessageBridge>          _bridge;
		OSFSettingsClient                       _osfSettings;

		// SFSE lifecycle producer -> main-thread consumer; DevTools is WndProc -> main.
		std::atomic_bool                        _dataLoadedInitPending{ false };
		std::atomic_bool              _devToolsRequested{ false };

		// Main owns the worker; its synchronized interface owns cross-thread jobs.
		std::unique_ptr<DevViewReloadWorker> _devViewReload;

		ViewPresentationController    _presentation;

		ViewOpenCoordinator _viewOpens;
		std::uint64_t _mainTickSerial{ 0 };
		std::unordered_map<std::string, std::uint32_t> _viewProtocolFaultCounts;

		bool _inputFocusGranted{ false };
		bool _menuEventsAttempted{ false };
		bool _menuEventsAvailable{ false };

		// Mutex-protected producer queue; only the main thread takes/applies batches.
		ViewRequestQueue m_viewRequests;
		ViewLoadTracker m_viewLoads;
		ViewInputGrants m_viewInputGrants;

		// WndProc produces cursor samples; main publishes geometry and also centers
		// the cursor on reveal. Both threads access only these atomic snapshots.
		std::atomic<float>         _cursorX{ 0.0f };
		std::atomic<float>         _cursorY{ 0.0f };
		std::atomic_bool           _cursorInsideView{ true };
		std::atomic_bool           _viewGeometryReady{ true };
		std::atomic<std::uint64_t> _captureSize{ PackViewSize(
			ViewSize{ kDefaultViewWidth, kDefaultViewHeight }) };
		std::atomic<std::uint64_t> _viewSize{ PackViewSize(
			ViewSize{ kDefaultViewWidth, kDefaultViewHeight }) };
		std::atomic_bool           _gameClientSizeObserved{ false };
		bool                       _fixedScaleformGeometry{ false };  // main thread
		
		static constexpr std::uint64_t kNoPendingMouseMove = ~0ull;
		std::atomic<std::uint64_t>     _pendingMouseMove{ kNoPendingMouseMove }; // WndProc/main -> main drain

		enum class RelativePointerStop : std::uint32_t
		{
			kNone = 0,
			kEnd = 1,
			kCancel = 2,
		};
		// Main owns the session; WndProc adds deltas and requests stops atomically.
		// Only main reads/writes the view id and invokes native pointer callbacks.
		std::atomic_bool                 _relativePointerActive{ false };
		std::atomic<float>               _relativePointerDx{ 0.0f };
		std::atomic<float>               _relativePointerDy{ 0.0f };
		std::atomic<float>               _relativePointerWheel{ 0.0f };
		std::atomic<RelativePointerStop> _relativePointerStop{ RelativePointerStop::kNone };
		std::string                      _relativePointerView;  // main-thread owner

		std::atomic_bool              _captureInput{ false }; // main -> WndProc
		bool                          _captureIntegrationInitialized{ false };
		bool                          _captureIntegrationAvailable{ false };
		std::atomic_bool              _postDataLoadedReady{ false }; // SFSE -> main
		bool                          _webRuntimeInitializing{ false };
		bool                          _webRuntimeReady{ false };

		std::atomic_bool              m_visible{ false }; // main -> WndProc/frame-event producer
		bool                          _rendererFailed{ false };  // opens fail closed while recovery is incomplete
		bool                          _rendererFailureLatched{ false };  // first failure per helper wins
		BrowserHostRecovery           _browserHostRecovery;
		bool                          _initialized{ false };
		bool                          _postPostLoadAttempted{ false };
		bool                          _developerMode{ false };  // startup-latched; setting changes apply next launch
		bool                          _highRefreshCapture{ false };  // startup-latched explicit 240 Hz opt-in

		ViewRevealGate                 m_viewReveal;

		std::string                   _lastShownView;

		bool                          _focusMenuOpen{ false };
		double                        _focusMenuMismatchSince{ -1.0 };

		XInputPoller                  m_gamepadSource;
		GamepadSession                 m_gamepadSession;

		ViewRecoveryTracker				m_viewRecovery;  // main-thread only; schedules and drives view reloads after load failures

		RetainedStateStore              _retainedState;

		
		std::string                     _lastViewsData;

		double _uptime{ 0.0 };
	};
}
