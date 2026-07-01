#pragma once

#include "composite/ICompositor.h"
#include "core/Config.h"
#include "input/InputRouter.h"
#include "render/IWebRenderer.h"
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
		// toggle key. Runs on the window-message thread.
		bool OnHostKey(std::uint32_t a_vkCode, bool a_down);

		// Called by the WndProc hook for each OS text character (WM_CHAR/
		// WM_UNICHAR), as a finished Unicode scalar value — layout-, dead-key-,
		// and AltGr-resolved, surrogate halves already combined. Routes into the
		// active web view while captured; ignored otherwise. The caller blocks
		// the character from the game itself. Runs on the window-message thread.
		void OnHostChar(std::uint32_t a_codepoint);

		// Called by the WndProc hook with RAW mouse deltas (the OS cursor is
		// hidden in gameplay). Advances a virtual cursor in view space and,
		// while captured, routes the move into the web view.
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

		// Fan-in point for input observers (UiInputHook). The router itself
		// only logs and drives the toggle path today.
		[[nodiscard]] InputRouter& Input() { return _input; }

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

		// Drain queued menu requests (F10/Esc/transition) on the main thread and apply the resulting policy. Called at the top of Tick.
		void DrainMenuRequests();

		// EXPERIMENTAL (config.focusMenu). Open/close the engine focus menu to
		// match overlay visibility. Called every tick from the game's MAIN thread
		// so the UIMessageQueue is never poked from the WndProc/input thread.
		// No-op unless config.focusMenu is set. See input/FocusMenu.h.
		void ReconcileFocusMenu();

		// EXPERIMENTAL (config.disableControls). Engage/release the engine
		// input-enable layer (device-agnostic control disable, incl. gamepad) to
		// match overlay visibility. Also main-thread-only. See input/ControlLayer.h.
		void ReconcileControlLayer();

		// Native reactions to settings changes (Phase 5b). Injected into the
		// settings module as its change listener; reacts only to the knobs core
		// owns (e.g. cursor speed).
		void OnSettingChanged(std::string_view a_modId, std::string_view a_key, const nlohmann::json& a_value);

		// Renderer load-lifecycle hook: a view's main frame finished or failed.
		// Called on the game thread from the renderer's notification pump.
		void OnViewLoad(std::string_view a_viewId, bool a_failed, std::string_view a_url,
			std::string_view a_description, int a_errorCode);

		Config                        _config;
		ViewManager                   _views;
		std::unique_ptr<IWebRenderer> _renderer;
		std::unique_ptr<ICompositor>  _compositor;
		std::unique_ptr<MessageBridge>          _bridge;
		std::vector<std::unique_ptr<IUiModule>> _modules;
		InputRouter                             _input;
		KeyCode                       _toggleKey{ kInvalidKeyCode };

		// Registered surfaces (menus/HUDs) + open state. Mutated only on the main thread (Tick / bridge handlers).
		MenuController                _menus;
		// Menu requests raised off the main thread, drained in Tick. _reqMutex is a strict LEAF lock: snapshot under it, release, then act.
		std::mutex                    _reqMutex;
		std::vector<MenuReq>          _reqs;

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
		std::atomic<float>            _cursorSpeed{ 1.0f };   // user multiplier (osfui.cursorSpeed)

		// Input-capture flag (initialised from config). When false the overlay
		// is a HUD: it draws but the game still gets input.
		std::atomic_bool              _captureInput{ true };

		std::atomic_bool              _visible{ false };
		bool                          _initialized{ false };

		// Last focus-menu open state we drove (main-thread only, reconciled in
		// Tick against _visible). EXPERIMENTAL — see config.focusMenu.
		bool                          _focusMenuOpen{ false };

		// Per-view load state (view id -> ViewLoadState), written from the
		// renderer's load hook and read by GetViewLoadState. Game-thread only.
		std::unordered_map<std::string, ViewLoadState> _viewLoadState;
	};
}
