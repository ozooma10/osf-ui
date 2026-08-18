#pragma once

#include <unordered_set>  // not in pch.h

#include "API/BridgeApi.h"
#include "Composite/D3D12Compositor.h"
#include "Core/Config.h"
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
#include "Runtime/UiModule.h"
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

		void OnGameWindowMouseAbsolute(int a_clientX, int a_clientY, int a_clientW, int a_clientH);
		void OnGameWindowMouseButton(int a_button, bool a_down);
		void OnGameWindowMouseWheel(int a_wheelDelta);

	private:
		friend class RuntimeHealthCoordinator;
		Runtime() = default;

		bool LoadRuntimeConfig();
		void LoadStartupContent();
		bool InitializeRenderer();
		void WireRendererLifecycleCallbacks();
		bool InitializeCompositor();
		void WireRenderPipeline();
		void InitializeFeatureModules();
		void InitializeBridge();
		void InitializeStartupViews();
		void ConfigureInputRouting();

		bool SetViewHidden(std::string_view a_id, bool a_hidden);
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
			std::vector<std::string>                 openViews;  // EnqueueOpenView (internal native triggers)
			std::vector<API::BridgeApi::ViewPresentationRequest> plugin;
		};
		PendingPresentationWork TakePresentationRequests();
		void                          PreparePresentationRequests(const PendingPresentationWork& a_work);
		void                          ApplyPresentationRequests(const PendingPresentationWork& a_work);

		bool BeginViewOpen(std::string_view a_id);
		bool CancelPendingOpen();
		void DrivePendingOpen();

		void DrainSchemaOps();

		void DrainViewRegistrations();

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
		void ProcessBackendQueues();
		void ProcessSettingsMaintenance();
		void ReconcileFrameState(double a_deltaSeconds);
		void ProcessRendererFrame(double a_deltaSeconds);

		void SyncLiveControlMapBindings();
		void SyncLiveControlMapHealth();
		// Invalidate and re-broadcast every projection that contains localized text after a locale/catalog change.
		void RefreshLocalizedData();

		void RefreshKeyboardLabels(const char* a_reason);
		std::string KeyLabelFor(std::string_view a_name) const;

		void DrainKeyCapture();
		void CancelArmedKeyCapture();

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

		// Publish one retained value to the mod's instantiated views.
		void PublishModState(std::string_view a_mod, std::string_view a_key, const nlohmann::json& a_value);

		// Publish one platform state key (settings/views/diagnostics/i18n plus the
		// current keybindings/engine-input-context state documents) to one greeted view, or to every greeted view when a_viewId is
		// empty. The i18n value is computed per view, since a view's catalog is
		// its owning mod's.
		void PublishPlatformState(std::string_view a_key, std::string_view a_viewId = {});

		// MessageBridge hello hook: a document greeted the bridge, `ready` is
		// already out, and its event gate is open. Replays every current state
		// value it is entitled to — platform keys plus its owning mod's.
		void OnViewGreeted(std::string_view a_viewId);

		// MessageBridge protocol-fault sink: routes faults to the view's console
		// in developer mode and raises health after repeated view misuse.
		void OnProtocolFault(std::string_view a_viewId, std::string_view a_code,
			std::string_view a_message, const nlohmann::json& a_detail, bool a_viewFault);

		Config                        _config;
		LocalizationService           _localization;
		ViewManager                   _views;
		std::unique_ptr<WebView2HostWebRenderer> _renderer;
		std::unique_ptr<D3D12Compositor> _compositor;
		std::unique_ptr<MessageBridge>          _bridge;
		std::vector<std::unique_ptr<IUiModule>> _modules;
		SettingsModule*                         _settings{ nullptr };  // owned by _modules; core reads schema facts through it
		HealthRegistry*                         _healthRegistry{ nullptr };  // owned by _modules
		RuntimeHealthCoordinator                _runtimeHealth{ *this };
		// Live key-typed bindings -> owner dispatch. Fed by OnGameWindowKey (window
		// thread), rebuilt from the store's listeners and drained in Tick (main
		// thread); wired in InitializeFeatureModules.
		HotkeyService                           _hotkeys;
		LiveControlMap                          _controlMap;
		DeferredMainThreadWork                  _controlMapInit;
		DeferredMainThreadWork                  _uiIntegrationInit;
		std::atomic<ScanCode>         _toggleKey{ kInvalidScanCode };
		std::atomic_bool              _devToolsRequested{ false };

		std::unique_ptr<DevViewReloadWorker> _devViewReload;

		// Instantiated views (menus/HUDs) + open state. Mutated only on the main
		// thread (Tick / bridge handlers).
		ViewPresentationController    _presentation;
		ViewPolicyStore               _viewPolicy;  // player HUD auto-start choices; main thread

		std::optional<std::string> _pendingViewOpen;

		// Last value pushed to WebView2HostWebRenderer::SetNativeFocus; the false
		// side posts a game-focus restore, so sends are edge-only. Main thread.
		bool _nativeFocusGranted{ false };

		ViewRequestQueue m_viewRequests;
		ViewLoadTracker m_viewLoads;
		ViewInputGrants m_viewInputGrants;

		// Virtual cursor in view-pixel space (the OS cursor is hidden during
		// gameplay, so raw deltas are accumulated instead). Position is written
		// by the WndProc (input) thread (plus the main-thread recenter on the
		// overlay-open edge); the view dims + cursor scale are written by the
		// render thread on resize and read by input, hence atomic.
		std::atomic<float>            _cursorX{ 0.0f };
		std::atomic<float>            _cursorY{ 0.0f };
		std::atomic<std::uint32_t>    _viewWidth{ kDefaultViewWidth };
		std::atomic<std::uint32_t>    _viewHeight{ kDefaultViewHeight };
		// Coalesced mouse-move handoff (QueueMouseMove -> Tick). OnGameWindowMouse*
		// fire per raw-input packet on the window thread; a pipe write per
		// packet made a 500-1000 Hz mouse cost hundreds of JSON encode/parse/
		// SendMouseInput round-trips per second while the page only samples at
		// display refresh. Instead the latest position is packed here (two
		// non-negative ints, so the all-bits-set sentinel can never collide)
		// and Tick injects at most one move per frame. Buttons/wheel stay
		// immediate — they carry their own coordinates, so a click between
		// ticks still lands at the right spot.
		static constexpr std::uint64_t kNoPendingMouseMove = ~0ull;
		std::atomic<std::uint64_t>     _pendingMouseMove{ kNoPendingMouseMove };
		// Coalescing telemetry: packets recorded (any thread) vs. moves sent
		// (main thread); logged and reset every few seconds in developer mode.
		std::atomic<std::uint32_t>     _mouseMovePackets{ 0 };
		std::uint32_t                  _mouseMoveSends{ 0 };
		double                         _nextMouseStatsLog{ 0.0 };

		// Derived from the active menu's manifest. A HUD or display-only menu
		// draws while leaving game input alone.
		std::atomic_bool              _captureInput{ false };
		// Set only after the game-layout guard, menu-event sink, FocusMenu
		// registration and game-window WndProc hook all succeed. Capturing menus
		// fail closed until the complete production input path is available; HUDs
		// remain usable.
		bool                          _captureIntegrationAvailable{ false };

		// _captureArmed is set on the main thread (the settings.captureKey send
		// endpoint) and read on the window thread (OnGameWindowKey); _capturedScan is
		// written on the window thread and drained on the main thread
		// (DrainKeyCapture) — both atomic. _captureView/_captureMod/_captureKey
		// (which view + setting to answer) and _captureUpScan (swallow the
		// captured key's release) are touched on a single thread each, so plain.
		// Localized keycap labels (RefreshKeyboardLabels): cache for the
		// changed-compare and for KeyLabelFor; the flag is set on the
		// window-message thread (WM_INPUTLANGCHANGE) and drained in Tick.
		KeyLabels                     _keyLabels;
		std::atomic_bool              _keyboardLayoutChanged{ false };

		std::atomic_bool              _captureArmed{ false };
		std::atomic<ScanCode>         _capturedScan{ kInvalidScanCode };
		std::string                   _captureView;   // main-thread: view that armed capture
		std::string                   _captureMod;    // main-thread: mod owning the setting being rebound
		std::string                   _captureKey;    // main-thread: which setting (e.g. "toggleKey")
		std::atomic<ScanCode>         _captureUpScan{ kInvalidScanCode };

		// Can the overlay actually reach the screen? `_overlayDrawAvailable` is
		// only the install-time half (the Scaleform vtable hooks); the UI pass's
		// command-list hooks are taken lazily on a render worker and their
		// self-test can disable drawing long afterwards. Every "may this open"
		// gate must ask both, or it admits an invisible overlay that still
		// captures focus and input.
		[[nodiscard]] bool OverlayCanDraw() const;

		std::atomic_bool              _visible{ false };
		std::atomic_bool              _overlayDrawAvailable{ false };
		bool                          _rendererFailed{ false };  // opens fail closed while recovery is incomplete
		bool                          _rendererFailureLatched{ false };  // first failure per helper wins
		BrowserHostRecovery           _browserHostRecovery;
		bool                          _initialized{ false };

		// Policy for revealing rendered view frames (main thread only)
		ViewRevealGate                 m_viewReveal;

		// The view shown as the overlay's active menu — the last one sent
		// ui.visibility{visible:true}. Any change (overlay close, menu.open view
		// switch) signals {visible:false} to this view first; by overlay close
		// ActiveMenu() is already empty, so the name must be tracked. Main-thread
		// only (ApplyViewPresentationPolicy).
		std::string                   _lastShownView;

		// Last focus-menu open state driven (main-thread only, reconciled in Tick
		// against the menu policy).
		bool                          _focusMenuOpen{ false };

		// Watchdog for the above (main-thread only): _uptime when the engine's
		// admitted state was first observed to disagree with _focusMenuOpen, or
		// <0 while they agree / a request is freshly in flight. kShow/kHide are
		// fire-and-forget UI-queue messages; if one is dropped the engine would
		// otherwise stay in menu mode forever with every control dead (bug report
		// 2026-07-20). ReconcileFocusMenu re-sends once the mismatch persists
		// past its grace window.
		double                        _focusMenuMismatchSince{ -1.0 };

		XInputPoller                  m_gamepadSource;
		GamepadSession                 m_gamepadSession;

		ViewRecoveryTracker				m_viewRecovery;  // main-thread only; schedules and drives view reloads after load failures

		// Retained mod state, shared by Papyrus SetView* and the native ABI's
		// SetViewState. Replayed to every document that greets the bridge, which
		// is what makes a mod-backend-fed view survive F5 with no lifecycle code.
		RetainedStateStore              _retainedState;
		// The last osfui/views value published (dedupe, so every ApplyViewPresentationPolicy
		// doesn't re-send an unchanged catalog). There is no subscriber set any
		// more: platform state goes to every greeted view.
		std::string                     _lastViewsData;
		std::unordered_map<std::string, std::uint32_t> _viewProtocolFaultCounts;
		double                          _nextLocalizationScan{ 0.0 };
		// Monotonic-ish plugin uptime accumulated from Tick's clamped dt; used
		// only to schedule recovery backoff (stalls with the game, which is the
		// cadence reloads should follow).
		double _uptime{ 0.0 };
	};
}
