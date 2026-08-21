#pragma once

#include <unordered_set>  // not in pch.h

#include "API/BridgeApi.h"
#include "API/PapyrusApi.h"
#include "Composite/D3D12Compositor.h"
#include "Input/GamepadSession.h"
#include "Input/KeyLabels.h"
#include "Render/WebView2HostWebRenderer.h"
#include "Diagnostics/HealthRegistry.h"
#include "Runtime/DeferredMainThreadWork.h"
#include "Views/Dev/DevViewReloadWorker.h"
#include "Bindings/HotkeyService.h"
#include "Localization/LocalizationService.h"
#include "Bindings/LiveControlMap.h"
#include "Views/ViewPresentationController.h"
#include "Render/BrowserHostRecovery.h"
#include "Runtime/RuntimeHealthCoordinator.h"
#include "Bridge/MessageBridge.h"
#include "Settings/SettingsModule.h"
#include "Input/ViewInputGrants.h"
#include "Views/ViewManager.h"
#include "Views/ViewLoadTracker.h"
#include "Views/ViewPolicyStore.h"
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
		// Install render hook after all peer plugins have chance to load & install hooks (so we can chan it)
		bool InstallOverlayDrawPath();
		void OnDataLoaded();
		void OnPostDataLoaded();

		void Tick(double a_deltaSeconds);

		bool IsVisible() const;

		void EnqueuePresentationRequest(ViewPresentationRequest a_req);

		void EnqueueOpenView(std::string a_viewId);

		//true when overlay owns input. pused to decide whether to consume game input and route into web view.
		bool IsInputCaptured() const;

		// Called by the WndProc hook on WM_KEYDOWN/WM_KEYUP (window-message thread):
		bool OnGameWindowKey(std::uint32_t a_vkCode, ScanCode a_scanCode, bool a_down);

		// Called by the WndProc hook on WM_INPUTLANGCHANGE (window-message thread): flags the keycap-label map for a main-thread rebuild.
		void NotifyKeyboardLayoutChanged();
		// Called by the WndProc hook when Starfield regains focus during an active capture.
		void NotifyGameWindowFocused();

		void OnGameWindowMouseAbsolute(int a_clientX, int a_clientY, int a_clientW, int a_clientH);
		// Accumulate one raw relative packet for the active ABI 1.10 owner. Returns true while a relative-pointer session owns wheel routing.
		bool OnGameWindowMouseRelative(int a_dx, int a_dy);
		void OnGameWindowMouseButton(int a_button, bool a_down);
		void OnGameWindowMouseWheel(int a_wheelDelta);

	private:
		friend class RuntimeHealthCoordinator;
		Runtime() = default;

		bool InitializePaths();
		void InitializeSettingsModule();
		void LoadLocalization();
		void LoadStartupContent();
		bool InitializeRenderer();
		void WireRendererLifecycleCallbacks();
		bool InitializeCompositor();
		void WireRenderPipeline();
		void InitializeFeatureModules();
		void InitializeBridge();
		void InitializeStartupViews();
		void ConfigureInputRouting();

		bool OnNativeAcceleratorKey(std::uint32_t a_vkCode, std::uint32_t a_scanCode, bool a_down);
		void OnOutputResized(std::uint32_t a_width, std::uint32_t a_height);
		void SubmitFrameIfVisible();

		void RegisterPlatformEndpoints(MessageBridge& a_bridge);

		bool InstantiateView(const ViewManifest& a_manifest, std::string_view a_reason);

		void ApplyViewPresentationPolicy();

		// Drive real OS focus toward the active-menu input session. HUD-only and closed states keep Starfield focused. Edge-guarded; main thread only.
		void ReconcileNativeFocus();

		void QueueMouseMove();

		struct PendingPresentationWork
		{
			std::vector<ViewPresentationRequest>         local;
			std::vector<ViewRequestQueue::OpenRequest>    openViews;  // EnqueueOpenView (internal native triggers)
			std::vector<API::BridgeApi::ViewPresentationRequest> plugin;
		};
		PendingPresentationWork TakePresentationRequests(std::vector<API::BridgeApi::ViewPresentationRequest> a_plugin);
		void                          PreparePresentationRequests(const PendingPresentationWork& a_work);
		void                          ApplyPresentationRequests(const PendingPresentationWork& a_work);

		bool BeginViewOpen(std::string_view a_id);
		bool CancelPendingOpen();
		void DrivePendingOpen();
		void BeginColdOpenTiming(std::string_view a_viewId, std::optional<std::chrono::steady_clock::time_point> a_requestedAt = std::nullopt);
		void CancelColdOpenTiming(std::string_view a_viewId);
		void FinishColdOpenTiming(std::string_view a_viewId);
		void BeginHiddenPrewarmTiming(std::string_view a_viewId);
		void CancelHiddenPrewarmTiming(std::string_view a_viewId);
		void FinishHiddenPrewarmTiming(std::string_view a_viewId, std::chrono::steady_clock::time_point a_loadedAt);

		void DrainViewRegistrations(std::vector<std::string> a_ids);
		void DrainSchemaOps(std::vector<API::BridgeApi::SchemaOp> a_ops);

		// open/close engine focus menu to match active menu capture policy.
		void ReconcileFocusMenu();

		void ReconcileSimPause();

		//Poll XInput and deliver events to active document (`ui.gamepad` events)
		void RouteGamepadInput(double a_deltaSeconds);

		void ReconcileControlLayer();

		void OnSettingChanged(std::string_view a_modId, std::string_view a_key, const nlohmann::json& a_value);

		void ApplyGameBindingConflictWarnings(bool a_enabled);
		void InitializeDataLoadedState();
		void InitializePostDataLoadIntegration();
		
		void ProcessLifecycleWork();
		void ProcessControlMapUpdates();
		void ProcessBackendQueues(API::Papyrus::PendingBatch a_papyrus, std::vector<API::BridgeApi::ViewStateOp> a_bridgeState);
		void ProcessSettingsMaintenance();
		void ProcessPauseMenuEntry();
		void ReconcileFrameState(double a_deltaSeconds);
		void ProcessRendererFrame(double a_deltaSeconds);

		void SyncLiveControlMapBindings();
		// Invalidate and re-broadcast every projection that contains localized text after a locale/catalog change.
		void RefreshLocalizedData();

		void RefreshKeyboardLabels(const char* a_reason);
		std::string KeyLabelFor(std::string_view a_name) const;

		void DrainKeyCapture();
		void CancelArmedKeyCapture();

		bool BeginRelativePointerCapture(std::string_view a_viewId);
		void EndRelativePointerCapture(std::string_view a_viewId);
		void CancelRelativePointerCapture(std::string_view a_viewId = {});
		void DrainRelativePointerCapture();
		void FinishRelativePointerCapture(API::RelativePointerPhase a_phase);

		void DrainHotkeys();

		void OnViewLoad(std::string_view a_viewId, bool a_failed, std::string_view a_url, std::string_view a_description, int a_errorCode);

		void ReloadViewInPlace(const std::string& a_id, const ViewManifest& a_manifest);

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
		void PublishPlatformState(std::string_view a_key, std::string_view a_viewId = {});

		void OnViewGreeted(std::string_view a_viewId);
		void OnProtocolFault(std::string_view a_viewId, std::string_view a_code, std::string_view a_message, const nlohmann::json& a_detail, bool a_viewFault);

		LocalizationService           _localization;
		ViewManager                   _views;
		std::unique_ptr<WebView2HostWebRenderer> _renderer;
		std::unique_ptr<D3D12Compositor> _compositor;
		std::unique_ptr<MessageBridge>          _bridge;
		std::unique_ptr<SettingsModule>          _settings;
		HealthRegistry                          _healthRegistry;
		RuntimeHealthCoordinator                _runtimeHealth{ *this };

		HotkeyService                           _hotkeys;
		LiveControlMap                          _controlMap;
		DeferredMainThreadWork                  _controlMapInit;
		DeferredMainThreadWork                  _uiIntegrationInit;
		std::atomic<ScanCode>         _toggleKey{ kInvalidScanCode };
		std::atomic_bool              _devToolsRequested{ false };

		std::unique_ptr<DevViewReloadWorker> _devViewReload;

		ViewPresentationController    _presentation;
		ViewPolicyStore               _viewPolicy;  // player HUD auto-start choices; main thread

		std::optional<std::string> _pendingViewOpen;

		using ViewTimingClock = std::chrono::steady_clock;
		struct ColdOpenTiming
		{
			std::string                              viewId;
			ViewTimingClock::time_point                requestedAt;
			std::optional<ViewTimingClock::time_point> instantiatedAt;
			std::optional<ViewTimingClock::time_point> loadedAt;
		};
		struct HiddenPrewarmTiming
		{
			std::string                              viewId;
			ViewTimingClock::time_point              requestedAt;
			std::optional<ViewTimingClock::time_point> instantiatedAt;
		};
		std::atomic<std::int64_t>     _lastToggleRequestNanos{ 0 };
		std::optional<ColdOpenTiming> _coldOpenTiming;
		std::optional<HiddenPrewarmTiming> _hiddenPrewarmTiming;

		bool _nativeFocusGranted{ false };
		std::atomic_bool _nativeFocusRefreshRequested{ false };

		ViewRequestQueue m_viewRequests;
		ViewLoadTracker m_viewLoads;
		ViewInputGrants m_viewInputGrants;

		std::atomic<float>            _cursorX{ 0.0f };
		std::atomic<float>            _cursorY{ 0.0f };
		std::atomic<std::uint32_t>    _viewWidth{ kDefaultViewWidth };
		std::atomic<std::uint32_t>    _viewHeight{ kDefaultViewHeight };
		
		static constexpr std::uint64_t kNoPendingMouseMove = ~0ull;
		std::atomic<std::uint64_t>     _pendingMouseMove{ kNoPendingMouseMove };

		enum class RelativePointerStop : std::uint32_t
		{
			kNone = 0,
			kEnd = 1,
			kCancel = 2,
		};
		std::atomic_bool                 _relativePointerActive{ false };
		std::atomic<float>               _relativePointerDx{ 0.0f };
		std::atomic<float>               _relativePointerDy{ 0.0f };
		std::atomic<float>               _relativePointerWheel{ 0.0f };
		std::atomic<RelativePointerStop> _relativePointerStop{ RelativePointerStop::kNone };
		std::string                      _relativePointerView;  // main-thread owner

		std::atomic_bool              _captureInput{ false };
		bool                          _captureIntegrationInitialized{ false };
		bool                          _captureIntegrationAvailable{ false };

		KeyLabels                     _keyLabels;
		std::atomic_bool              _keyboardLayoutChanged{ false };

		std::atomic_bool              _captureArmed{ false };
		std::atomic<ScanCode>         _capturedScan{ kInvalidScanCode };
		std::string                   _captureView;   // main-thread: view that armed capture
		std::string                   _captureMod;    // main-thread: mod owning the setting being rebound
		std::string                   _captureKey;    // main-thread: which setting (e.g. "toggleKey")
		std::atomic<ScanCode>         _captureUpScan{ kInvalidScanCode };

		bool OverlayCanDraw() const;

		std::atomic_bool              m_visible{ false };
		bool                          _rendererFailed{ false };  // opens fail closed while recovery is incomplete
		bool                          _rendererFailureLatched{ false };  // first failure per helper wins
		BrowserHostRecovery           _browserHostRecovery;
		bool                          _initialized{ false };
		bool                          _developerMode{ false };  // startup-latched; setting changes apply next launch
		bool                          _highRefreshCapture{ false };  // startup-latched explicit 240 Hz opt-in

		ViewRevealGate                 m_viewReveal;
		std::optional<FrameBufferView> _latestFrame;

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
