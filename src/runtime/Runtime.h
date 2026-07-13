#pragma once

#include <unordered_set>  // not in pch.h

#include "api/BridgeApi.h"
#include "composite/ICompositor.h"
#include "core/Config.h"
#include "input/InputRouter.h"
#include "render/IWebRenderer.h"
#include "runtime/HotkeyService.h"
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

		// Advances the renderer and submits a frame when visible.
		//
		// Called every frame on the game's Main thread via an SFSE permanent
		// task (core/Plugin.cpp). Runs under SFSE's task-queue lock: keep it
		// cheap, never block. Exact cadence at main menu / pause is still
		// unverified in-game (docs/reverse-engineering-notes.md).
		void Tick(double a_deltaSeconds);

		[[nodiscard]] bool IsVisible() const;

		enum class MenuReq
		{
			ToggleDefault,  // F10: open the default menu, or close the top one
			CloseTop,       // Esc: close the top menu
			CloseAll,       // transition/panic: clear every surface
		};
		void EnqueueMenuRequest(MenuReq a_req);

		// Open one registered surface by id on the next tick (any thread; same
		// policy path as the plugin API's RequestMenu). Used by internal
		// native triggers — e.g. the injected PauseMenu "mod settings" entry.
		void EnqueueOpenView(std::string a_viewId);

		// Show/hide one loaded (config.views) declarative view by id, independent
		// of the global overlay toggle. Returns false for an unknown/unloaded id.
		// Drives the renderer's per-view hidden flag.
		bool SetViewHidden(std::string_view a_id, bool a_hidden);

		// Per-view load lifecycle (internal hook). A view is Loading until its
		// main frame finishes (Finished) or errors out (Failed). A failed load
		// never reaches DOM-ready, so this is
		// the authoritative "did the view come up" signal and the groundwork for
		// URL crash-recovery. Read on the game thread.
		enum class ViewLoadState { Loading, Finished, Failed };
		[[nodiscard]] ViewLoadState GetViewLoadState(std::string_view a_id) const;

		// True when the overlay currently owns input: visible AND config
		// captureInput is on. Read by the WndProc hook (OverlayInputHook) to
		// decide whether to consume game input, and by the InputRouter to
		// decide whether to route keys into the web view. Thread-safe.
		[[nodiscard]] bool IsInputCaptured() const;

		// Called by the WndProc hook for each keyboard transition (Windows VK
		// code). Drives the toggle key and, while captured, routes the key
		// into the web view. Returns true if the caller should CONSUME the key
		// (i.e. not pass it to the game) — true while captured or for the
		// toggle key. The configured console key is the exception: it is never
		// consumed and never routed, so the game's console still opens while the
		// overlay is up (the overlay is dismissed so it doesn't hide the console).
		// Runs on the window-message thread.
		bool OnHostKey(std::uint32_t a_vkCode, bool a_down);

		// Called by the WndProc hook for each OS text character (WM_CHAR/
		// WM_UNICHAR), as a finished Unicode scalar value — layout-, dead-key-,
		// and AltGr-resolved, surrogate halves already combined. Routes into the
		// active web view while captured; ignored otherwise. The caller blocks
		// the character from the game itself. Runs on the window-message thread.
		void OnHostChar(std::uint32_t a_codepoint);

		// Called by the WndProc hook with the OS pointer's position while the
		// HARDWARE cursor drives the overlay (config.hardwareCursor, default):
		// window-client coordinates plus the current client size. Maps through
		// the client size to view space (the view is aspect-matched but
		// height-capped — a uniform scale), syncs the virtual cursor so buttons/
		// wheel route at the same spot, and routes the move into the web view.
		void OnHostMouseAbsolute(int a_clientX, int a_clientY, int a_clientW, int a_clientH);

		// FALLBACK path (config.hardwareCursor=false): called by the WndProc
		// hook with RAW mouse deltas (the OS cursor stays hidden). Advances a
		// virtual cursor in view space and, while captured, routes the move
		// into the web view.
		void OnHostMouseDelta(int a_dx, int a_dy);
		// Mouse button transition; routed at the current virtual cursor.
		// a_button uses MouseButton order (0=left, 1=right, 2=middle).
		void OnHostMouseButton(int a_button, bool a_down);
		// Mouse wheel; routed at the current virtual cursor. a_wheelDelta is a
		// signed multiple of WHEEL_DELTA (120): positive = wheel forward/up.
		void OnHostMouseWheel(int a_wheelDelta);

		// Called by the compositor (render thread) when the output surface
		// size is known/changes. Resizes the web view to match the screen
		// (aspect-correct, height-capped) so the page renders undistorted, and
		// rescales cursor sensitivity to the new view size.
		void OnOutputResized(std::uint32_t a_width, std::uint32_t a_height);

		// Renders and submits one frame if the overlay is visible. Split out
		// from Tick so a future present-side hook can drive submission at a
		// different cadence than logic updates.
		void SubmitFrameIfVisible();

		[[nodiscard]] MessageBridge* Bridge() { return _bridge.get(); }
		[[nodiscard]] const Config&  GetConfig() const { return _config; }

	private:
		Runtime() = default;

		std::unique_ptr<IWebRenderer> CreateRenderer() const;
		std::unique_ptr<ICompositor>  CreateCompositor() const;

		// Composition root for feature modules (settings, future HUD, …) and
		// the platform's own bridge commands. Core wires modules here but stays
		// ignorant of what each one does past the IUiModule contract.
		void BuildModules();
		void RegisterPlatformCommands(MessageBridge& a_bridge);

		// Derive the desired UI state from the MenuController and apply it to the renderer/compositor/flags (hidden, order, active view, capture, visibility).
		void ApplyMenuPolicy();

		// Queued menu requests, snapshotted at the top of Tick (F10/Esc/
		// transition plus the native API's RequestMenu ops) and APPLIED after
		// BridgeApi::PumpMainThread. The snapshot-first/apply-after split is the
		// host half of the ABI 1.3 delivery guarantee: any SendToWeb a consumer
		// issued BEFORE a RequestMenu in this snapshot is flushed to the view's
		// queue by the pump before the open unhides the view — so the page
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
		// UnregisterSettingsSchema ops to the store (Source::kNative) on the
		// main thread. Called from Tick BEFORE BridgeApi::PumpMainThread so a
		// registration's value replay reaches SubscribeSettings consumers the
		// same tick.
		void DrainSchemaOps();

		// EXPERIMENTAL (config.focusMenu). Open/close the engine focus menu to
		// match the top menu's capture policy. Called every tick from the game's
		// MAIN thread so the UIMessageQueue is never poked from the WndProc/input
		// thread. No-op unless config.focusMenu is set. See input/FocusMenu.h.
		void ReconcileFocusMenu();

		// Drive the sim pause (Main::isGameMenuPaused) toward the top menu's
		// pausesGame manifest policy. Unconditional (no config gate — needs no
		// engine menu), every tick, main thread. See input/SimPause.h.
		void ReconcileSimPause();

		// EXPERIMENTAL (config.engineInput). Increment 3: drain the engine's
		// per-menu GAMEPAD input (marshalled by EngineInput from worker threads)
		// on the main thread and route it into the active web view — default
		// mapping (D-pad/left-stick -> arrows, A -> Enter, B -> close overlay,
		// right-stick -> scroll) plus raw `ui.gamepad` bridge events. No-op
		// unless engineInput is set. Keyboard/mouse stay on the WndProc path.
		void DrainEngineInput(double a_deltaSeconds);

		// Engage/release the engine input-enable layer (device-agnostic control
		// disable, incl. gamepad) to match the top menu's capture policy. On by
		// default; config.disableControls is a diagnostic escape hatch (config.json
		// only, like hardwareCursor). Main-thread-only. See input/ControlLayer.h.
		void ReconcileControlLayer();

		// Native reactions to settings changes (Phase 5b). Injected into the
		// settings module as its change listener; reacts only to the knobs core
		// owns (e.g. cursor speed).
		void OnSettingChanged(std::string_view a_modId, std::string_view a_key, const nlohmann::json& a_value);

		// (Re)apply _toggleKey to the input router with the standard
		// toggle/close callbacks. Called at init and after a live rebind.
		void ApplyToggleKey();

		// Key-rebind capture. The settings view arms capture via the
		// `settings.captureKey` command; the NEXT key press is grabbed in
		// OnHostKey (window thread, consumed so it can't also toggle/close) into
		// _capturedVk, and DrainKeyCapture (main thread, from Tick) maps it to a
		// name and sends `settings.captured` back to the view — which then does a
		// normal settings.set so persistence/validation/re-resolution reuse the
		// existing path.
		void DrainKeyCapture();

		// Deliver hotkey fires queued by OnHostKey (window thread) to both
		// consumption channels on the main thread (mcm-design.md §9): the C
		// ABI's SubscribeHotkey queue (invoked by BridgeApi::PumpMainThread
		// later the same tick) and the settings module's `ui.hotkey` web push.
		void DrainHotkeys();

		// Renderer load-lifecycle hook: a view's main frame finished or failed.
		// Called on the game thread from the renderer's notification pump.
		void OnViewLoad(std::string_view a_viewId, bool a_failed, std::string_view a_url,
			std::string_view a_description, int a_errorCode);

		// URL crash-recovery (P2): fire due reload attempts scheduled by
		// OnViewLoad. Called from Tick on the game thread.
		void DriveRecovery();

		// The `views.data` catalog (bridge 0.2): one entry per REGISTERED surface
		// with its manifest metadata + live open/focus/load state. Read-only
		// snapshot; a view torn down by crash-recovery drops out (unregistered).
		[[nodiscard]] nlohmann::json BuildViewsData() const;

		// Re-send `views.data` to every view that has requested it (`views.get`
		// subscribes the caller), but only when the catalog actually changed —
		// callers invoke this unconditionally after any potential state change
		// (ApplyMenuPolicy, OnViewLoad). Main thread only.
		void BroadcastViewsData();

		Config                        _config;
		ViewManager                   _views;
		std::unique_ptr<IWebRenderer> _renderer;
		std::unique_ptr<ICompositor>  _compositor;
		std::unique_ptr<MessageBridge>          _bridge;
		std::vector<std::unique_ptr<IUiModule>> _modules;
		SettingsModule*                         _settings{ nullptr };  // owned by _modules; core reads schema facts through it
		InputRouter                             _input;
		// Live key-typed bindings -> owner dispatch (mcm-design.md §9). Fed by
		// OnHostKey (window thread), rebuilt from the store's listeners and
		// drained in Tick (main thread); wired in BuildModules.
		HotkeyService                           _hotkeys;
		KeyCode                       _toggleKey{ kInvalidKeyCode };
		KeyCode                       _consoleKey{ kInvalidKeyCode };  // passed through to the game; see OnHostKey

		// Registered surfaces (menus/HUDs) + open state. Mutated only on the main thread (Tick / bridge handlers).
		MenuController                _menus;
		// Menu requests raised off the main thread, drained in Tick. _reqMutex is a strict LEAF lock: snapshot under it, release, then act.
		std::mutex                    _reqMutex;
		std::vector<MenuReq>          _reqs;
		std::vector<std::string>      _openViewReqs;  // EnqueueOpenView, same lock/drain discipline

		// Virtual cursor in view-pixel space (the OS cursor is hidden during
		// gameplay, so we accumulate raw deltas instead). Position is touched
		// only by the WndProc (input) thread; the view dims + cursor scale are
		// written by the render thread on resize and read by input, so they're
		// atomic.
		float                         _cursorX{ 0.0f };
		float                         _cursorY{ 0.0f };
		std::atomic<std::uint32_t>    _viewWidth{ 1280 };
		std::atomic<std::uint32_t>    _viewHeight{ 720 };
		std::atomic<float>            _cursorScale{ 1.0f };   // resolution-based, set on resize

		// Input-capture flag (initialised from config). When false the overlay
		// is a HUD: it draws but the game still gets input.
		std::atomic_bool              _captureInput{ true };

		// Key-rebind capture state. _captureArmed is set on the main thread (the
		// settings.captureKey command) and read on the window thread (OnHostKey);
		// _capturedVk is written on the window thread and drained on the main
		// thread (DrainKeyCapture) — both atomic. _captureView/_captureMod/
		// _captureKey (which view + setting to answer) and _captureUpVk (swallow
		// the captured key's release) are touched on a single thread each, so plain.
		std::atomic_bool              _captureArmed{ false };
		std::atomic<KeyCode>          _capturedVk{ kInvalidKeyCode };
		std::string                   _captureView;   // main-thread: view that armed capture
		std::string                   _captureMod;    // main-thread: mod owning the setting being rebound
		std::string                   _captureKey;    // main-thread: which setting (e.g. "toggleKey")
		KeyCode                       _captureUpVk{ kInvalidKeyCode };  // window-thread only

		std::atomic_bool              _visible{ false };
		bool                          _initialized{ false };

		// Deferred compositor reveal (main thread only). The present-hook
		// compositor keeps drawing its last cached texture while visible, so on
		// the closed->open edge ApplyMenuPolicy arms this instead of calling
		// SetVisible(true): SubmitFrameIfVisible holds the reveal until the
		// renderer hands over a frame with a NEW serial — one produced after the
		// open, i.e. after every queued message was delivered (ABI 1.3
		// message-before-first-paint). Costs at most a couple of frames of open
		// latency; prevents any flash of stale pre-open overlay content.
		bool          _revealPending{ false };
		std::uint64_t _lastSubmittedFrame{ 0 };

		// The view that last received ui.visibility{visible:true}, so the open->closed edge can
		// signal the SAME view {visible:false} (by then ActiveMenu() is already empty). Main-thread
		// only (ApplyMenuPolicy).
		std::string                   _lastShownView;

		// Last focus-menu open state we drove (main-thread only, reconciled in
		// Tick against the menu policy). EXPERIMENTAL — see config.focusMenu.
		bool                          _focusMenuOpen{ false };

		// Gamepad routing state (main-thread only; DrainEngineInput). Left-stick
		// nav uses per-direction repeat timers (0=up,1=down,2=left,3=right;
		// value 0 = inactive/fresh sentinel, else next-fire time in _uptime
		// seconds). Right-stick scroll accumulates fractional notches. Sticks
		// send raw bridge events only when they change past an epsilon.
		double                        _padNavNextRepeat[4]{};
		float                         _padScrollAccum{ 0.0f };
		float                         _padLastSentSticks[4]{};  // lx,ly,rx,ry last sent as raw bridge event

		// Per-view load state (view id -> ViewLoadState), written from the
		// renderer's load hook and read by GetViewLoadState. Game-thread only.
		std::unordered_map<std::string, ViewLoadState> _viewLoadState;

		// URL crash-recovery (P2). A failed main-frame load schedules bounded
		// reloads with backoff; exhaustion destroys the view and unregisters its
		// surface so nothing can reopen a dead view. attempts counts reloads
		// already fired; a successful load clears the entry. Game-thread only.
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
		std::string                     _lastViewsData;
		// Monotonic-ish plugin uptime accumulated from Tick's clamped dt; only
		// used to schedule recovery backoff (stalls with the game, which is
		// exactly the cadence reloads should follow).
		double _uptime{ 0.0 };
	};
}
