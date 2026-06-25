#pragma once

#include "api/ViewRegistry.h"
#include "composite/ICompositor.h"
#include "core/Config.h"
#include "input/InputRouter.h"
#include "render/IWebRenderer.h"
#include "runtime/MessageBridge.h"
#include "runtime/SettingsModule.h"
#include "runtime/UiModule.h"
#include "runtime/ViewManager.h"

namespace PrismaSF
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

		void SetVisible(bool a_visible);
		void ToggleVisible();
		[[nodiscard]] bool IsVisible() const;

		// Show/hide one loaded (config.views) declarative view by id, independent
		// of the global overlay toggle. Returns false for an unknown/unloaded id.
		// Drives the renderer's per-view hidden flag — the same mechanism the
		// consumer API uses for programmatic views.
		bool SetViewHidden(std::string_view a_id, bool a_hidden);

		// Per-view load lifecycle (internal hook; NOT exposed to the consumer
		// API). A view is Loading until its main frame finishes (Finished) or
		// errors out (Failed). A failed load never reaches DOM-ready, so this is
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

		// --- Public consumer API (src/api/PrismaUI_API.h -> PRISMA_UI_API) ----
		// Backs the exported RequestPluginAPI vtable. Each maps a PrismaView
		// handle to a programmatically-created view. Safe to call from any thread:
		// the handle table is locked, the renderer queues its work, and callbacks
		// are delivered back on the game thread. Queries return immediately;
		// mutations take effect on the renderer's next worker pass. Prefer calling
		// from the game main thread (e.g. an SFSE message handler or a per-frame
		// task), as PrismaUI consumers do.
		std::uint64_t      ApiCreateView(std::string a_htmlPath, ViewRegistry::DomReadyCb a_onDomReady);
		void               ApiDestroy(std::uint64_t a_handle);
		[[nodiscard]] bool ApiIsValid(std::uint64_t a_handle) const;
		void ApiInvoke(std::uint64_t a_handle, std::string a_script, IWebRenderer::ScriptResultHandler a_onResult);
		void ApiInteropCall(std::uint64_t a_handle, std::string a_fn, std::string a_arg);
		void ApiRegisterJSListener(std::uint64_t a_handle, std::string a_name, IWebRenderer::JsListenerHandler a_cb);
		void ApiRegisterConsoleCallback(std::uint64_t a_handle, ViewRegistry::ConsoleCb a_cb);
		[[nodiscard]] bool ApiHasFocus(std::uint64_t a_handle) const;
		bool               ApiFocus(std::uint64_t a_handle, bool a_pauseGame, bool a_disableFocusMenu);
		void               ApiUnfocus(std::uint64_t a_handle);
		[[nodiscard]] bool ApiHasAnyActiveFocus() const;
		void               ApiShow(std::uint64_t a_handle);
		void               ApiHide(std::uint64_t a_handle);
		[[nodiscard]] bool ApiIsHidden(std::uint64_t a_handle) const;
		void               ApiSetOrder(std::uint64_t a_handle, int a_order);
		[[nodiscard]] int  ApiGetOrder(std::uint64_t a_handle) const;
		void               ApiSetScrollingPixelSize(std::uint64_t a_handle, int a_px);
		[[nodiscard]] int  ApiGetScrollingPixelSize(std::uint64_t a_handle) const;

	private:
		Runtime() = default;

		std::unique_ptr<IWebRenderer> CreateRenderer() const;
		std::unique_ptr<ICompositor>  CreateCompositor() const;

		// Composition root for feature modules (settings, future HUD, …) and
		// the platform's own bridge commands. Core wires modules here but stays
		// ignorant of what each one does past the IUiModule contract.
		void BuildModules();
		void RegisterPlatformCommands(MessageBridge& a_bridge);

		// Cycle the active (input) view among the interactive ones — the focusKey
		// handler. No-op with fewer than two interactive views.
		void CycleActiveView();

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
		KeyCode                       _focusKey{ kInvalidKeyCode };
		// Focusable (interactive) view ids in load order, and the index of the
		// one that currently has input. Touched only on the window-message thread.
		std::vector<std::string>      _interactiveViews;
		std::size_t                   _activeViewIndex{ 0 };

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
		std::atomic<float>            _cursorSpeed{ 1.0f };   // user multiplier (prismasf.cursorSpeed)

		// Input-capture flag (initialised from config). When false the overlay
		// is a HUD: it draws but the game still gets input.
		std::atomic_bool              _captureInput{ true };

		std::atomic_bool              _visible{ false };
		bool                          _initialized{ false };

		// Last focus-menu open state we drove (main-thread only, reconciled in
		// Tick against _visible). EXPERIMENTAL — see config.focusMenu.
		bool                          _focusMenuOpen{ false };

		// Programmatically-created (consumer-API) views: handle <-> internal-id
		// table + per-view logical state. Thread-safe; see src/api/ViewRegistry.h.
		ViewRegistry                  _apiViews;

		// Per-view load state (view id -> ViewLoadState), written from the
		// renderer's load hook and read by GetViewLoadState. Game-thread only.
		std::unordered_map<std::string, ViewLoadState> _viewLoadState;
		// Set by ApiFocus(disableFocusMenu): suppresses the engine focus menu in
		// ReconcileFocusMenu even when config.focusMenu is on. Cleared on Unfocus.
		std::atomic_bool              _apiSuppressFocusMenu{ false };
	};
}
