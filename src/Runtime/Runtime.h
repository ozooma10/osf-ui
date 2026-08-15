#pragma once

#include <unordered_set>  // not in pch.h

#include "API/BridgeApi.h"
#include "Composite/ICompositor.h"
#include "Core/Config.h"
#include "Input/GamepadNavigation.h"
#include "Input/KeyLabels.h"
#include "Render/IWebRenderer.h"
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
#include "Views/ViewManager.h"
#include "Views/ViewLifecycle.h"
#include "Views/ViewPolicyStore.h"
#include "Views/ViewRevealGate.h"
#include "Bridge/RetainedStateStore.h"

namespace OSFUI
{
	class Runtime
	{
	public:
		[[nodiscard]] static Runtime& Get();

		bool Initialize();
		// Install the render-pass seam after SFSE has loaded every peer plugin.
		// Luma must patch the vanilla ScaleformComposite implementation before
		// OSF UI chains it; calling this during our own Plugin_Load is too early.
		bool InstallOverlayDrawPath();
		// SFSE kPostDataLoad may be dispatched from a job thread. Publish a
		// notification only; Tick consumes it at its proven main-thread checkpoint.
		void OnDataLoaded();
		// Same handoff for UI integration that becomes legal at kPostPostDataLoad.
		void OnPostDataLoaded();

		// Advances the renderer and submits a frame when visible. Called on the
		// game main thread through RE::BSService::TaskQueue; an SFSE permanent
		// task running on a render-graph worker is only the coalesced producer.
		// Keep it cheap and never block.
		void Tick(double a_deltaSeconds);

		[[nodiscard]] bool IsVisible() const;

		enum class PresentationRequest
		{
			ToggleDefault,  // F10: open the default menu, or close the top one
			Back,           // Esc / pad-B: delegate to a back-owning view, else close the active menu
			CloseAll,       // transition/panic: close every view
		};
		void EnqueuePresentationRequest(PresentationRequest a_req);

		// Open one discovered view by id on the next tick (any thread; same
		// policy path as the plugin API's RequestMenu). Used by internal native
		// triggers — e.g. the injected PauseMenu "mod settings" entry.
		void EnqueueOpenView(std::string a_viewId);

		// True when the overlay owns input. Read by the WndProc hook
		// (OverlayInputHook) to decide whether to
		// consume game input and by OnGameWindowKey to decide whether to route keys into
		// the web view. Thread-safe.
		[[nodiscard]] bool IsInputCaptured() const;

		// Called by the WndProc hook for each keyboard transition. a_vkCode is
		// the message's Windows VK (what the web layer consumes); a_scanCode is
		// the composed physical code (Input/ScanCode.h) — binding identity:
		// toggle match, hotkey dispatch, and rebind capture all key on it.
		// Drives the toggle key and, while captured, routes the key into the
		// web view. Returns true if the caller should consume the key — while
		// captured or for the toggle key. Runs on the window-message thread.
		bool OnGameWindowKey(std::uint32_t a_vkCode, ScanCode a_scanCode, bool a_down);

		// Called by the WndProc hook on WM_INPUTLANGCHANGE (window-message
		// thread): flags the keycap-label map for a main-thread rebuild.
		void NotifyKeyboardLayoutChanged();

		// WndProc hook, hardware-cursor path:
		// window-client coordinates plus the current client size. Maps through
		// the client size to view space (aspect-matched but height-capped — a
		// uniform scale), syncs the virtual cursor so buttons/wheel route at the
		// same spot, and routes the move into the web view.
		void OnGameWindowMouseAbsolute(int a_clientX, int a_clientY, int a_clientW, int a_clientH);

		// Mouse button transition; routed at the current virtual cursor.
		// a_button uses MouseButton order (0=left, 1=right, 2=middle).
		void OnGameWindowMouseButton(int a_button, bool a_down);
		// Mouse wheel; routed at the current virtual cursor. a_wheelDelta is a
		// signed multiple of WHEEL_DELTA (120): positive = wheel forward/up.
		void OnGameWindowMouseWheel(int a_wheelDelta);

		[[nodiscard]] const Config&  GetConfig() const { return _config; }

	private:
		friend class RuntimeHealthCoordinator;
		Runtime() = default;

		bool LoadRuntimeConfig();
		void LoadStartupContent();

		// Internally owned renderer and load-state edges.
		bool SetViewHidden(std::string_view a_id, bool a_hidden);
		enum class ViewLoadState { Loading, Finished, Failed };
		[[nodiscard]] ViewLoadState GetViewLoadState(std::string_view a_id) const;
		bool OnNativeAcceleratorKey(std::uint32_t a_vkCode, std::uint32_t a_scanCode, bool a_down);
		void OnOutputResized(std::uint32_t a_width, std::uint32_t a_height);
		void SubmitFrameIfVisible();

		// Composition root for feature modules (settings, health) and the
		// platform's own bridge endpoints. Ownership is through the base type —
		// `_modules` holds `unique_ptr<IUiModule>` — while `_settings`/`_healthRegistry`
		// keep non-owning concrete-typed pointers the core reaches through directly.
		// Both are real: the modules are driven polymorphically through the shared
		// IUiModule lifecycle loops in registration order (health last), AND
		// named concretely at the ~38 sites that need module-specific facts.
		void BuildModules();
		void RegisterPlatformEndpoints(MessageBridge& a_bridge);

		// Instantiate and add one discovered view with exactly the same
		// renderer/console/bridge/load-state wiring at boot, RegisterView time,
		// and first open. Idempotent for an already-instantiated view.
		bool InstantiateView(const ViewManifest& a_manifest, std::string_view a_reason);

		// Derive the desired UI state from ViewPresentationController and apply it to the
		// renderer/compositor/flags (hidden, order, input target, capture,
		// visibility).
		void ApplyViewPresentationPolicy();

		// Drive real OS focus toward the active-menu input session. HUD-only and
		// closed states keep Starfield focused. Edge-guarded; main thread only.
		void ReconcileNativeFocus();

		// Record the current virtual-cursor position as the pending coalesced
		// mouse move (window thread for raw packets, main thread for the
		// overlay-open placement). Tick flushes it as one InjectMouseMove.
		void QueueMouseMove();

		// Queued presentation requests, snapshotted at the top of Tick (F10/Esc/
		// transition plus the native API's RequestMenu ops) and applied after
		// BridgeApi::PumpMainThread. The snapshot-first/apply-after split is the
		// OSF UI runtime half of the ABI 1.3 delivery guarantee: any SendToWeb a consumer
		// issued before a RequestMenu in this snapshot is flushed to the view's
		// queue by the pump before the open unhides the view, so the page
		// observes the message before its first visible paint.
		struct PendingPresentationWork
		{
			std::vector<PresentationRequest>         local;
			std::vector<std::string>                 openViews;  // EnqueueOpenView (internal native triggers)
			std::vector<API::BridgeApi::ViewPresentationRequest> plugin;
		};
		[[nodiscard]] PendingPresentationWork TakePresentationRequests();
		void                          PreparePresentationRequests(const PendingPresentationWork& a_work);
		void                          ApplyPresentationRequests(const PendingPresentationWork& a_work);

		// First-open handoff: keep a newly-created menu hidden until its page is
		// usable. Fast loads open directly; slower loads temporarily show the
		// pinned osfui/handoff view with the target menu's input/pause
		// policy. Views may opt into an explicit `view.ready` milestone.
		bool BeginViewOpen(std::string_view a_id);
		bool CancelPendingOpen();
		void RetryPendingOpen();
		void DrivePendingOpen();
		void ShowHandoff(std::string_view a_phase, bool a_retry);
		void FinishPendingOpen();

		// Apply the native API's queued RegisterSettingsSchema /
		// UnregisterSettingsSchema ops to the store (Source::kNative) on the main
		// thread. Called from Tick before BridgeApi::PumpMainThread so a
		// registration's value replay reaches SubscribeSettings consumers the
		// same tick.
		void DrainSchemaOps();

		// Apply the native plugin API's queued RegisterView ids (ABI 1.5): validate
		// each boot-discovered views/<modId>/<viewName>/ manifest, instantiating only
		// openOnStart views.
		// Called before the menu-request snapshot so RegisterView -> SendToWeb ->
		// RequestMenu issued back-to-back all land in one tick. Main thread.
		void DrainViewRegistrations();

		// Open/close the engine focus menu to match the active menu's capture
		// policy. Called every tick from the main thread so the
		// UIMessageQueue is never poked from the WndProc/input thread. See
		// Input/FocusMenu.h.
		void ReconcileFocusMenu();

		// Drive the sim pause (Main::isGameMenuPaused) toward the active menu's
		// pausesGame manifest policy. Unconditional (no config gate — needs no
		// engine menu), every tick, main thread. See Input/SimPause.h.
		void ReconcileSimPause();

		// Drain the engine's per-menu gamepad input
		// (marshalled by EngineInput from worker threads) on the main thread and
		// route it into the active menu's document — default mapping (D-pad/left-stick
		// -> arrows, A -> Enter, B -> close overlay, right-stick -> scroll) plus
		// raw `ui.gamepad` bridge events.
		// Keyboard/mouse stay on the WndProc path.
		void DrainEngineInput(double a_deltaSeconds);

		// Engage/release the engine input-enable layer (device-agnostic control
		// disable, incl. gamepad) to match the active menu's capture policy. No
		// config gate. Main-thread-only. See Input/ControlLayer.h.
		void ReconcileControlLayer();

		// Injected into the settings module as its change listener; reacts only
		// to the knobs core owns (e.g. cursor speed).
		void OnSettingChanged(std::string_view a_modId, std::string_view a_key, const nlohmann::json& a_value);

		// Toggle game-binding conflict warnings without hiding or discarding the current
		// read-only game catalog. Re-broadcasts `osfui/settings` state because per-setting
		// conflict annotations are carried there.
		void ApplyGameBindingConflictWarnings(bool a_enabled);
		void InitializeDataLoadedState();
		void InitializePostDataLoadIntegration();
		void SyncLiveControlMapBindings();
		void SyncLiveControlMapHealth();
		// Invalidate and re-broadcast every projection that contains localized
		// text after a locale/catalog change.
		void RefreshLocalizedData();

		// Rebuild the localized keycap-label map (KeyLabels) for the game
		// window thread's current layout and publish it into the settings doc
		// when it changed. Main thread. Triggers: startup, WM_INPUTLANGCHANGE
		// (via NotifyKeyboardLayoutChanged), locale change, capture arm.
		void RefreshKeyboardLabels(const char* a_reason);
		// The current layout's label for a canonical key name; the name itself
		// when unknown. Main thread (reads the RefreshKeyboardLabels cache).
		[[nodiscard]] std::string KeyLabelFor(std::string_view a_name) const;

		// Key-rebind capture. `settings.captureKey` arms it; the next key press is
		// grabbed in OnGameWindowKey (window thread, consumed so it can't also toggle/
		// close) into _capturedScan, and DrainKeyCapture (main thread, from Tick)
		// maps it to a name and sends `settings.captured` back to the view. The
		// view answers with a normal settings.set, so persistence/validation/
		// re-resolution reuse the existing path.
		void DrainKeyCapture();
		// Cancels a still-armed rebind capture (settings.captureKey answered with
		// cancelled:true) when the menu goes away under it — the mouse "Exit"
		// button, pad-B, or a transition CloseAll. Without this the next gameplay
		// keypress is swallowed and silently committed as the new binding.
		void CancelArmedKeyCapture();

		// Deliver hotkey fires queued by OnGameWindowKey (window thread) to both
		// consumption channels on the main thread: the C ABI's SubscribeHotkey
		// queue (invoked by BridgeApi::PumpMainThread later the same tick) and
		// the settings module's `ui.hotkey` web push.
		void DrainHotkeys();

		// Renderer load-lifecycle hook: a view's main frame finished or failed.
		// Called on the game thread from the renderer's notification pump.
		void OnViewLoad(std::string_view a_viewId, bool a_failed, std::string_view a_url,
			std::string_view a_description, int a_errorCode);

		// Has this view reached its reveal gate? A manifest that declares readySignal
		// reaches it only once the document signals content readiness (osfui.markReady);
		// everything else reaches it when main-frame loading finishes. a_state is the caller's already-resolved
		// GetViewLoadState, so this does not re-look it up.
		[[nodiscard]] bool IsViewRevealReady(std::string_view a_id, const ViewManifest& a_manifest, ViewLoadState a_state) const;

		// Reload one view's URL in place: mark it Loading, clear its content-ready state,
		// IWebRenderer::CreateOrNavigateView, then restore the output-matched size.
		// The shared core of crash-recovery, dev-reload, and pending-open retry.
		// The renderer must
		// exist — every caller guards _renderer first.
		void ReloadViewInPlace(const std::string& a_id, const ViewManifest& a_manifest);

		// Fire due reload attempts scheduled by OnViewLoad. Called from Tick on
		// the game thread.
		void DriveRecovery();

		// Suspend hidden views after a short grace and reclaim non-pinned views after
		// a much longer idle period (or past the hidden-view cap). The pure policy
		// lives in ViewLifecycle; this method applies due actions to instantiated
		// Runtime/renderer state.
		void DriveViewLifecycle();

		// Player-configurable automatic start is reserved for catalog-visible
		// HUDs: catalog-hidden (`hub:false`) views cannot silently run in the background, and
		// debugOnly views qualify only while developer mode is on. Pinned core
		// views are resident anyway and never configurable.
		[[nodiscard]] bool HudAutoStartEligible(const ViewManifest& a_manifest) const;
		enum class ViewTeardownReason
		{
			LoadExhausted,
			IdleReclaim,
		};
		void TearDownView(const std::string& a_id, ViewTeardownReason a_reason);

		// DevTools request raised by F12 on the window/browser-host thread. Resolve the
		// active menu and talk to the renderer from Tick on the game thread.
		void DriveDevTools();

		// Publish instantiated views to the worker and drain completed mirror refreshes.
		// Navigation remains on the game thread.
		void PumpDevViewReload();
		// The `osfui/views` catalog state: one entry per discovered view
		// with its manifest metadata + current open/active-menu/main-frame-load state. Read-only
		// snapshot; a view torn down by crash recovery remains discovered but becomes uninstantiated.
		[[nodiscard]] nlohmann::json BuildViewsData() const;

		// A terminal renderer-instance failure closes every view and immediately
		// releases all menu-owned engine policy. Browser-host connection failures schedule
		// bounded recovery; security/runtime repair failures remain disabled.
		void OnRendererFailure(const IWebRenderer::FailureEvent& a_event);
		// Deferred until the failure callback has returned; recreates every instantiated view.
		void DriveBrowserHostRecovery();
		void RehydrateRendererAfterRestart();

		// Re-publish the `osfui/views` state key, but only when the catalog
		// changed — callers invoke this unconditionally after any potential
		// state change (ApplyViewPresentationPolicy, OnViewLoad). Main thread only.
		void BroadcastViewsData();

		// Every instantiated view of one mod ("<modId>/..."), the delivery target set
		// for that mod's state and events. Derived fresh each time, so nothing
		// can go stale.
		[[nodiscard]] std::unordered_set<std::string> InstantiatedViewsOfMod(std::string_view a_mod) const;

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

		// MessageBridge protocol-fault sink: bridge/API-detected faults for a view.
		// Routes to that view's own console in developer mode and raises the
		// `view.protocol-misuse` health issue once it repeats.
		void OnProtocolFault(std::string_view a_viewId, std::string_view a_code,
			std::string_view a_message, const nlohmann::json& a_detail, bool a_viewFault);

		Config                        _config;
		LocalizationService           _localization;
		ViewManager                   _views;
		std::unique_ptr<IWebRenderer> _renderer;
		std::unique_ptr<ICompositor>  _compositor;
		std::unique_ptr<MessageBridge>          _bridge;
		std::vector<std::unique_ptr<IUiModule>> _modules;
		SettingsModule*                         _settings{ nullptr };  // owned by _modules; core reads schema facts through it
		HealthRegistry*                      _healthRegistry{ nullptr };  // owned by _modules
		RuntimeHealthCoordinator                      _runtimeHealth{ *this };
		// Live key-typed bindings -> owner dispatch. Fed by OnGameWindowKey (window
		// thread), rebuilt from the store's listeners and drained in Tick (main
		// thread); wired in BuildModules.
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
		ViewLifecycle                 _viewLifecycle;
		ViewPolicyStore               _viewPolicy;  // player HUD auto-start choices; main thread
		std::unordered_set<std::string> _pinnedViews;
		struct PendingViewOpen
		{
			std::string target;
			double      startedAt{ 0.0 };
			double      loadedAt{ -1.0 };
			std::string phase;
			bool        handoffVisible{ false };
			bool        error{ false };
			bool        retryRequested{ false };
		};
		std::optional<PendingViewOpen> _pendingViewOpen;
		// Explicit readiness is page-lifetime state. Cleared before every
		// navigation and set only by that page's `view.ready` send endpoint.
		std::unordered_set<std::string> _contentReadyViews;

		// Views holding the gamepad raw-passthrough grant (osfui.gamepadRaw).
		// Sticky per view: survives overlay hide/show, cleared on page (re)load
		// and view destroy. DrainEngineInput applies the active menu's flag each
		// tick. Main thread only.
		std::unordered_set<std::string> _gamepadRawViews;
		// Views owning the back action (osfui.handleBack): while such a view is
		// the active menu, Esc / pad-B are delegated to the page as a synthetic
		// Escape instead of closing the active menu (the page navigates, peels an
		// inner panel, or sends `close` itself). Same stickiness/cleanup rules
		// as _gamepadRawViews. Main thread only.
		std::unordered_set<std::string> _backOwnerViews;
		// Last value pushed to IWebRenderer::SetNativeFocus; the false
		// side posts a game-focus restore, so sends are edge-only. Main thread.
		bool _nativeFocusGranted{ false };
		// Presentation requests raised off the main thread, drained in Tick. _reqMutex is
		// a strict leaf lock: snapshot under it, release, then act.
		std::mutex                    _reqMutex;
		std::vector<PresentationRequest> _presentationRequests;
		std::vector<std::string>      _openViewReqs;  // EnqueueOpenView, same lock/drain discipline

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
		// only the install-time half (the Scaleform vtable hooks); the seam's
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

		// Gamepad routing state (main-thread only; DrainEngineInput). Left-stick
		// navigation is a single latched direction with release hysteresis and
		// delayed hold-repeat. Right-stick scroll accumulates fractional notches.
		// Sticks send raw bridge events only when they change past an epsilon.
		GamepadNavigation             _padNavigation;
		float                         _padScrollAccum{ 0.0f };
		float                         _padLastSentSticks[4]{};  // lx,ly,rx,ry last sent as raw bridge event
		// When the WebView owns foreground focus, Starfield's engine gamepad
		// feed is suspended. XInput is polled directly for that interval; the
		// first sample is a baseline so the button that opened the menu cannot
		// leak through as an activation.
		bool                          _directPadActive{ false };
		std::uint32_t                 _directPadButtons{ 0 };

		// Written from the renderer's load hook, read by GetViewLoadState.
		// Game-thread only.
		std::unordered_map<std::string, ViewLoadState> _viewLoadState;

		// URL crash-recovery. A failed main-frame load schedules bounded reloads
		// with backoff; exhaustion destroys and removes the instantiated view
		// so nothing can reopen a dead view. attempts counts reloads already
		// fired; a successful load clears the entry. Game-thread only.
		struct RecoveryState
		{
			std::uint32_t attempts{ 0 };
			double        retryAt{ 0.0 };  // in _uptime seconds
			bool          pending{ false };
		};
		std::unordered_map<std::string, RecoveryState> _recovery;

		// Retained mod state, shared by Papyrus SetView* and the native ABI's
		// SetViewState. Replayed to every document that greets the bridge, which
		// is what makes a mod-backend-fed view survive F5 with no lifecycle code.
		RetainedStateStore              _retainedState;
		// The last osfui/views value published (dedupe, so every ApplyViewPresentationPolicy
		// doesn't re-send an unchanged catalog). There is no subscriber set any
		// more: platform state goes to every greeted view.
		std::string                     _lastViewsData;
		// Latest handoff-view state, republished on that view's greeting so an
		// F5 mid-handoff does not strand it on its cold pre-state look.
		nlohmann::json                  _handoffState;
		// Per-view count of view-caused protocol faults; crossing the threshold
		// raises one `view.protocol-misuse` health issue.
		std::unordered_map<std::string, std::uint32_t> _viewProtocolFaultCounts;
		double                          _nextLocalizationScan{ 0.0 };
		// Monotonic-ish plugin uptime accumulated from Tick's clamped dt; used
		// only to schedule recovery backoff (stalls with the game, which is the
		// cadence reloads should follow).
		double _uptime{ 0.0 };
	};
}
