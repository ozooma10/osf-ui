#pragma once

#include <unordered_set>  // not in pch.h

#include "api/BridgeApi.h"
#include "composite/ICompositor.h"
#include "core/Config.h"
#include "input/InputRouter.h"
#include "render/IWebRenderer.h"
#include "runtime/HotkeyService.h"
#include "runtime/LocalizationService.h"
#include "runtime/MenuController.h"
#include "runtime/MessageBridge.h"
#include "runtime/SettingsModule.h"
#include "runtime/UiModule.h"
#include "runtime/ViewManager.h"

namespace OSFUI
{
	// Owns the whole plugin runtime: config, views, renderer, compositor,
	// bridge, input, and the overlay visibility state. Constructed and
	// initialized from SFSE_PLUGIN_LOAD.
	class Runtime
	{
	public:
		[[nodiscard]] static Runtime& Get();

		bool Initialize();
		void Shutdown();

		// Advances the renderer and submits a frame when visible. Called every
		// frame on the game's Main thread via an SFSE permanent task, under
		// SFSE's task-queue lock: keep it cheap, never block. Cadence at main
		// menu / pause is unverified in-game.
		void Tick(double a_deltaSeconds);

		[[nodiscard]] bool IsVisible() const;

		enum class MenuReq
		{
			ToggleDefault,  // F10: open the default menu, or close the top one
			Back,           // Esc / pad-B: delegate to a back-owning view, else close the top menu
			CloseTop,       // close the top menu unconditionally
			CloseAll,       // transition/panic: clear every surface
		};
		void EnqueueMenuRequest(MenuReq a_req);

		// Open one registered surface by id on the next tick (any thread; same
		// policy path as the plugin API's RequestMenu). Used by internal native
		// triggers — e.g. the injected PauseMenu "mod settings" entry.
		void EnqueueOpenView(std::string a_viewId);

		// Show/hide one loaded (config.views) declarative view by id, independent
		// of the global overlay toggle. Returns false for an unknown/unloaded id.
		// Drives the renderer's per-view hidden flag.
		bool SetViewHidden(std::string_view a_id, bool a_hidden);

		// Per-view load lifecycle. Loading until the main frame finishes or
		// errors out. A failed load never reaches DOM-ready, so this is the
		// authoritative "did the view come up" signal. Read on the game thread.
		enum class ViewLoadState { Loading, Finished, Failed };
		[[nodiscard]] ViewLoadState GetViewLoadState(std::string_view a_id) const;

		// True when the overlay owns input: visible and config captureInput is
		// on. Read by the WndProc hook (OverlayInputHook) to decide whether to
		// consume game input, and by the InputRouter to decide whether to route
		// keys into the web view. Thread-safe.
		[[nodiscard]] bool IsInputCaptured() const;

		// Called by the WndProc hook for each keyboard transition (Windows VK
		// code). Drives the toggle key and, while captured, routes the key into
		// the web view. Returns true if the caller should consume the key —
		// while captured or for the toggle key. The configured console key is
		// the exception: never consumed and never routed, so the game's console
		// still opens while the overlay is up (the overlay is dismissed so it
		// doesn't hide the console). Runs on the window-message thread.
		bool OnHostKey(std::uint32_t a_vkCode, bool a_down);
		// Called by WebView2's AcceleratorKeyPressed hook on its STA worker.
		// Returns true only for framework-owned keys; ordinary typing remains
		// unhandled so Chromium receives real Win32 keyboard and IME input.
		bool OnNativeAcceleratorKey(std::uint32_t a_vkCode, bool a_down);

		// WndProc hook, one OS text character (WM_CHAR/WM_UNICHAR) as a finished
		// Unicode scalar value — layout-, dead-key- and AltGr-resolved, surrogate
		// halves already combined. Routes into the active web view while
		// captured; ignored otherwise. The caller blocks the character from the
		// game itself. Runs on the window-message thread.
		void OnHostChar(std::uint32_t a_codepoint);

		// WndProc hook, hardware-cursor path (config.hardwareCursor, default):
		// window-client coordinates plus the current client size. Maps through
		// the client size to view space (aspect-matched but height-capped — a
		// uniform scale), syncs the virtual cursor so buttons/wheel route at the
		// same spot, and routes the move into the web view.
		void OnHostMouseAbsolute(int a_clientX, int a_clientY, int a_clientW, int a_clientH);

		// Fallback path (config.hardwareCursor=false): raw mouse deltas from the
		// WndProc hook (the OS cursor stays hidden). Advances a virtual cursor in
		// view space and, while captured, routes the move into the web view.
		void OnHostMouseDelta(int a_dx, int a_dy);
		// Mouse button transition; routed at the current virtual cursor.
		// a_button uses MouseButton order (0=left, 1=right, 2=middle).
		void OnHostMouseButton(int a_button, bool a_down);
		// Mouse wheel; routed at the current virtual cursor. a_wheelDelta is a
		// signed multiple of WHEEL_DELTA (120): positive = wheel forward/up.
		void OnHostMouseWheel(int a_wheelDelta);

		// Called by the compositor (render thread) when the output surface size
		// is known/changes. Resizes the web view to match the screen
		// (aspect-correct, height-capped) so the page renders undistorted, and
		// rescales cursor sensitivity to the new view size.
		void OnOutputResized(std::uint32_t a_width, std::uint32_t a_height);

		// Split out from Tick so a present-side hook can drive submission at a
		// different cadence than logic updates.
		void SubmitFrameIfVisible();

		[[nodiscard]] MessageBridge* Bridge() { return _bridge.get(); }
		[[nodiscard]] const Config&  GetConfig() const { return _config; }

	private:
		Runtime() = default;

		std::unique_ptr<IWebRenderer> CreateRenderer() const;
		std::unique_ptr<ICompositor>  CreateCompositor() const;

		// Composition root for feature modules (settings, future HUD, …) and the
		// platform's own bridge commands. Core knows only the IUiModule contract.
		void BuildModules();
		void RegisterPlatformCommands(MessageBridge& a_bridge);

		// Derive the desired UI state from the MenuController and apply it to the
		// renderer/compositor/flags (hidden, order, active view, capture,
		// visibility).
		void ApplyMenuPolicy();

		// Drive real OS keyboard focus toward the active view's text-entry grant
		// (focus-on-demand). Edge-guarded; main thread only.
		void ReconcileNativeFocus();

		// Record the current virtual-cursor position as the pending coalesced
		// mouse move (window thread for raw packets, main thread for the
		// overlay-open placement). Tick flushes it as one InjectMouseMove.
		void QueueMouseMove();

		// Queued menu requests, snapshotted at the top of Tick (F10/Esc/
		// transition plus the native API's RequestMenu ops) and applied after
		// BridgeApi::PumpMainThread. The snapshot-first/apply-after split is the
		// host half of the ABI 1.3 delivery guarantee: any SendToWeb a consumer
		// issued before a RequestMenu in this snapshot is flushed to the view's
		// queue by the pump before the open unhides the view, so the page
		// observes the message before its first visible paint.
		struct PendingMenuWork
		{
			std::vector<MenuReq>                     local;
			std::vector<std::string>                 openViews;  // EnqueueOpenView (internal native triggers)
			std::vector<API::BridgeApi::MenuRequest> plugin;
		};
		[[nodiscard]] PendingMenuWork TakeMenuRequests();
		void                          ApplyMenuRequests(const PendingMenuWork& a_work);

		// Apply the native API's queued RegisterSettingsSchema /
		// UnregisterSettingsSchema ops to the store (Source::kNative) on the main
		// thread. Called from Tick before BridgeApi::PumpMainThread so a
		// registration's value replay reaches SubscribeSettings consumers the
		// same tick.
		void DrainSchemaOps();

		// Apply the native plugin API's queued RegisterView ids (ABI 1.5): load
		// a boot-discovered views/<id>/ manifest that config.views didn't list
		// and register it as an openable surface. Called from Tick before the
		// menu-request snapshot so RegisterView -> SendToWeb -> RequestMenu
		// issued back-to-back all land in one tick. Main thread.
		void DrainViewRegistrations();

		// config.focusMenu: open/close the engine focus menu to match the top
		// menu's capture policy. Called every tick from the main thread so the
		// UIMessageQueue is never poked from the WndProc/input thread. No-op
		// unless config.focusMenu is set. See input/FocusMenu.h.
		void ReconcileFocusMenu();

		// Drive the sim pause (Main::isGameMenuPaused) toward the top menu's
		// pausesGame manifest policy. Unconditional (no config gate — needs no
		// engine menu), every tick, main thread. See input/SimPause.h.
		void ReconcileSimPause();

		// config.engineInput: drain the engine's per-menu gamepad input
		// (marshalled by EngineInput from worker threads) on the main thread and
		// route it into the active web view — default mapping (D-pad/left-stick
		// -> arrows, A -> Enter, B -> close overlay, right-stick -> scroll) plus
		// raw `ui.gamepad` bridge events. No-op unless engineInput is set.
		// Keyboard/mouse stay on the WndProc path.
		void DrainEngineInput(double a_deltaSeconds);

		// Engage/release the engine input-enable layer (device-agnostic control
		// disable, incl. gamepad) to match the top menu's capture policy. No
		// config gate. Main-thread-only. See input/ControlLayer.h.
		void ReconcileControlLayer();

		// Injected into the settings module as its change listener; reacts only
		// to the knobs core owns (e.g. cursor speed).
		void OnSettingChanged(std::string_view a_modId, std::string_view a_key, const nlohmann::json& a_value);

		// (Re)apply _toggleKey to the input router with the standard
		// toggle/close callbacks. Called at init and after a live rebind.
		void ApplyToggleKey();

		// Build (or clear) the vanilla-keys conflict table to match the
		// osfui.vanillaKeyConflicts setting (MCM-owned, toggles live). Lazy: the
		// table loads on the first enable, so a persisted "off" never pays the
		// parse. Re-broadcasts settings.data (the conflict annotations live in
		// the settings document). Main thread.
		void ApplyVanillaKeyConflicts(bool a_enabled);
		// Invalidate and re-broadcast every projection that contains localized
		// text after a locale/catalog change.
		void RefreshLocalizedData();

		// Key-rebind capture. `settings.captureKey` arms it; the next key press is
		// grabbed in OnHostKey (window thread, consumed so it can't also toggle/
		// close) into _capturedVk, and DrainKeyCapture (main thread, from Tick)
		// maps it to a name and sends `settings.captured` back to the view. The
		// view answers with a normal settings.set, so persistence/validation/
		// re-resolution reuse the existing path.
		void DrainKeyCapture();

		// Deliver hotkey fires queued by OnHostKey (window thread) to both
		// consumption channels on the main thread: the C ABI's SubscribeHotkey
		// queue (invoked by BridgeApi::PumpMainThread later the same tick) and
		// the settings module's `ui.hotkey` web push.
		void DrainHotkeys();

		// Renderer load-lifecycle hook: a view's main frame finished or failed.
		// Called on the game thread from the renderer's notification pump.
		void OnViewLoad(std::string_view a_viewId, bool a_failed, std::string_view a_url,
			std::string_view a_description, int a_errorCode);

		// Fire due reload attempts scheduled by OnViewLoad. Called from Tick on
		// the game thread.
		void DriveRecovery();

		// Dev view-reload: reload the top open menu's URL in place when
		// _devReloadRequested was raised. Same LoadView + Resize pair as
		// crash-recovery. Called from Tick on the game thread.
		void DriveDevReload();

		// The `views.data` catalog (bridge 0.2): one entry per registered surface
		// with its manifest metadata + live open/focus/load state. Read-only
		// snapshot; a view torn down by crash-recovery drops out (unregistered).
		[[nodiscard]] nlohmann::json BuildViewsData() const;

		// Re-send `views.data` to every view that requested it (`views.get`
		// subscribes the caller), but only when the catalog changed — callers
		// invoke this unconditionally after any potential state change
		// (ApplyMenuPolicy, OnViewLoad). Main thread only.
		void BroadcastViewsData();

		Config                        _config;
		LocalizationService           _localization;
		ViewManager                   _views;
		std::unique_ptr<IWebRenderer> _renderer;
		std::unique_ptr<ICompositor>  _compositor;
		std::unique_ptr<MessageBridge>          _bridge;
		std::vector<std::unique_ptr<IUiModule>> _modules;
		SettingsModule*                         _settings{ nullptr };  // owned by _modules; core reads schema facts through it
		InputRouter                             _input;
		// Live key-typed bindings -> owner dispatch. Fed by OnHostKey (window
		// thread), rebuilt from the store's listeners and drained in Tick (main
		// thread); wired in BuildModules.
		HotkeyService                           _hotkeys;
		KeyCode                       _toggleKey{ kInvalidKeyCode };
		bool                          _vanillaKeysApplied{ false };  // main-thread; ApplyVanillaKeyConflicts edge detector
		// Dev view-reload: resolved from config devReloadKey only when devMode,
		// so kInvalid doubles as the gate. The window thread raises the flag
		// (OnHostKey), Tick drains it (DriveDevReload — renderer calls are
		// main-thread).
		KeyCode                       _devReloadKey{ kInvalidKeyCode };
		std::atomic_bool              _devReloadRequested{ false };

		// Registered surfaces (menus/HUDs) + open state. Mutated only on the main
		// thread (Tick / bridge handlers).
		MenuController                _menus;

		// Views holding the gamepad raw-passthrough grant (osfui.gamepadRaw).
		// Sticky per view: survives overlay hide/show, cleared on page (re)load
		// and view destroy. DrainEngineInput applies the active view's flag each
		// tick. Main thread only.
		std::unordered_set<std::string> _gamepadRawViews;
		// Views owning the back action (osfui.handleBack): while such a view is
		// the active menu, Esc / pad-B are delegated to the page as a synthetic
		// Escape instead of closing the top menu (the page navigates, peels an
		// inner panel, or sends `close` itself). Same stickiness/cleanup rules
		// as _gamepadRawViews. Main thread only.
		std::unordered_set<std::string> _backOwnerViews;
		// Views whose page reported live text entry (osfui.textFocus, sent by
		// the host bridge shim on real typing intent — a click into an editable
		// or a printable keystroke inside one, NOT mere DOM focus, which padnav
		// moves on every dpad step). While the active menu holds this grant the
		// WebView owns real OS keyboard focus (typing/IME); otherwise the game
		// window keeps focus, because Windows.Gaming.Input — Starfield's
		// gamepad source — stops delivering the moment another process owns
		// the focused window (2026-07-21 report: gamepad dead all session under
		// WebView2's take-focus-on-open model). Cleared on page (re)load, view
		// destroy, and overlay close. Main thread only.
		std::unordered_set<std::string> _textFocusViews;
		// Last value pushed to IWebRenderer::SetNativeKeyboardFocus; the false
		// side posts a game-focus restore, so sends are edge-only. Main thread.
		bool _nativeFocusGranted{ false };
		// Menu requests raised off the main thread, drained in Tick. _reqMutex is
		// a strict leaf lock: snapshot under it, release, then act.
		std::mutex                    _reqMutex;
		std::vector<MenuReq>          _reqs;
		std::vector<std::string>      _openViewReqs;  // EnqueueOpenView, same lock/drain discipline

		// Virtual cursor in view-pixel space (the OS cursor is hidden during
		// gameplay, so raw deltas are accumulated instead). Position is written
		// by the WndProc (input) thread (plus the main-thread recenter on the
		// overlay-open edge); the view dims + cursor scale are written by the
		// render thread on resize and read by input, hence atomic.
		float                         _cursorX{ 0.0f };
		float                         _cursorY{ 0.0f };
		std::atomic<std::uint32_t>    _viewWidth{ kDefaultViewWidth };
		std::atomic<std::uint32_t>    _viewHeight{ kDefaultViewHeight };
		std::atomic<float>            _cursorScale{ 1.0f };   // resolution-based, set on resize

		// Coalesced mouse-move handoff (QueueMouseMove -> Tick). OnHostMouse*
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
		// (main thread); logged and reset every few seconds in devMode.
		std::atomic<std::uint32_t>     _mouseMovePackets{ 0 };
		std::uint32_t                  _mouseMoveSends{ 0 };
		double                         _nextMouseStatsLog{ 0.0 };

		// Initialised from config. When false the overlay is a HUD: it draws but
		// the game still gets input.
		std::atomic_bool              _captureInput{ true };

		// _captureArmed is set on the main thread (the settings.captureKey
		// command) and read on the window thread (OnHostKey); _capturedVk is
		// written on the window thread and drained on the main thread
		// (DrainKeyCapture) — both atomic. _captureView/_captureMod/_captureKey
		// (which view + setting to answer) and _captureUpVk (swallow the captured
		// key's release) are touched on a single thread each, so plain.
		std::atomic_bool              _captureArmed{ false };
		std::atomic<KeyCode>          _capturedVk{ kInvalidKeyCode };
		std::string                   _captureView;   // main-thread: view that armed capture
		std::string                   _captureMod;    // main-thread: mod owning the setting being rebound
		std::string                   _captureKey;    // main-thread: which setting (e.g. "toggleKey")
		std::string                   _captureRequestId;  // main-thread: arming request's id, echoed on settings.captured
		std::atomic<KeyCode>          _captureUpVk{ kInvalidKeyCode };

		std::atomic_bool              _visible{ false };
		bool                          _initialized{ false };

		// Deferred compositor reveal (main thread only). The present-hook
		// compositor keeps drawing its last cached texture while visible, so on
		// the closed->open edge ApplyMenuPolicy arms this instead of calling
		// SetVisible(true): SubmitFrameIfVisible holds the reveal until the
		// renderer hands over a frame with a new serial — one produced after the
		// open, i.e. after every queued message was delivered (ABI 1.3
		// message-before-first-paint). D3D12 additionally waits until Present has
		// reported the output size and the renderer has painted at that size.
		// Costs at most a couple of frames of open latency; prevents flashes of
		// stale or manifest-resolution content.
		bool          _revealPending{ false };
		bool          _revealFrameReady{ false };
		std::uint64_t _lastSubmittedFrame{ 0 };

		// The view shown as the overlay's focused menu — the last one sent
		// ui.visibility{visible:true}. Any change (overlay close, menu.open view
		// switch) signals {visible:false} to this view first; by overlay close
		// ActiveMenu() is already empty, so the name must be tracked. Main-thread
		// only (ApplyMenuPolicy).
		std::string                   _lastShownView;

		// Last focus-menu open state driven (main-thread only, reconciled in Tick
		// against the menu policy). See config.focusMenu.
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
		// nav uses per-direction repeat timers (0=up,1=down,2=left,3=right;
		// value 0 = inactive/fresh sentinel, else next-fire time in _uptime
		// seconds). Right-stick scroll accumulates fractional notches. Sticks
		// send raw bridge events only when they change past an epsilon.
		double                        _padNavNextRepeat[4]{};
		float                         _padScrollAccum{ 0.0f };
		float                         _padLastSentSticks[4]{};  // lx,ly,rx,ry last sent as raw bridge event

		// Written from the renderer's load hook, read by GetViewLoadState.
		// Game-thread only.
		std::unordered_map<std::string, ViewLoadState> _viewLoadState;

		// URL crash-recovery. A failed main-frame load schedules bounded reloads
		// with backoff; exhaustion destroys the view and unregisters its surface
		// so nothing can reopen a dead view. attempts counts reloads already
		// fired; a successful load clears the entry. Game-thread only.
		struct RecoveryState
		{
			std::uint32_t attempts{ 0 };
			double        retryAt{ 0.0 };  // in _uptime seconds
			bool          pending{ false };
		};
		std::unordered_map<std::string, RecoveryState> _recovery;

		// views.data change-push state (main-thread only): which views asked for
		// the catalog (and so get updates), and the last payload sent (dedupe so
		// every ApplyMenuPolicy doesn't re-send an unchanged catalog).
		std::unordered_set<std::string> _viewsSubscribers;
		// view id -> requested localization domain (normally its owning mod).
		std::unordered_map<std::string, std::string> _i18nSubscribers;
		std::string                     _lastViewsData;
		double                          _nextLocalizationScan{ 0.0 };
		// Monotonic-ish plugin uptime accumulated from Tick's clamped dt; used
		// only to schedule recovery backoff (stalls with the game, which is the
		// cadence reloads should follow).
		double _uptime{ 0.0 };
	};
}
