#include "runtime/Runtime.h"

#include <cmath>

#include "RE/C/Calendar.h"

#include "api/BridgeApi.h"
#include "composite/D3D12Compositor.h"
#include "composite/NullCompositor.h"
#include "core/Log.h"
#include "input/ControlLayer.h"
#include "input/FocusMenu.h"
#include "input/HardwareCursor.h"
#include "core/Paths.h"
#include "platform/WindowsPlatform.h"
#include "render/MockWebRenderer.h"
#include "runtime/Json.h"
#include "render/NullWebRenderer.h"
#include "render/UltralightWebRenderer.h"

namespace OSFUI
{
	Runtime& Runtime::Get()
	{
		static Runtime instance;
		return instance;
	}

	bool Runtime::Initialize()
	{
		if (_initialized) {
			return true;
		}

		if (!Paths::Initialize()) {
			return false;
		}

		_config = Config::Load(Paths::ConfigFile());
		Log::SetDevMode(_config.devMode);

		if (!_config.enabled) {
			REX::INFO("Runtime: disabled via config; nothing further will be initialized");
			return true;
		}

		_views.LoadAll(Paths::ViewsDir());

		// Renderer
		_renderer = CreateRenderer();
		const auto* view = _views.Find(_config.view);
		const auto initialWidth = view ? view->width : 1280u;
		const auto initialHeight = view ? view->height : 720u;
		_viewWidth.store(initialWidth);
		_viewHeight.store(initialHeight);
		_cursorX = initialWidth * 0.5f;
		_cursorY = initialHeight * 0.5f;
		RendererConfig rendererConfig{
			.width = initialWidth,
			.height = initialHeight,
			.devMode = _config.devMode,
			.dataDir = Paths::DataDir(),
		};
		if (!_renderer->Initialize(rendererConfig)) {
			REX::ERROR("Runtime: renderer '{}' failed to initialize; falling back to null renderer", _renderer->Name());
			_renderer = std::make_unique<NullWebRenderer>();
			_renderer->Initialize(rendererConfig);
		}
		REX::INFO("Runtime: renderer = {}", _renderer->Name());

		// Route per-view load finish/fail to the internal core hook. A failed
		// load never fires DOM-ready, so this is the only signal a view didn't
		// come up — groundwork for crash-recovery.
		_renderer->SetLoadHandler([this](const IWebRenderer::LoadEvent& a_e) {
			OnViewLoad(a_e.viewId, a_e.failed, a_e.url, a_e.description, a_e.errorCode);
		});

		// The active page's CSS `cursor` drives the real OS pointer (hover
		// hand, text I-beam, …). NOTE: unlike the other handlers this fires on
		// the renderer's WORKER thread (IWebRenderer.h contract) — SetShape is
		// one atomic store, applied by the WndProc hook on the next mouse
		// message.
		if (_config.hardwareCursor) {
			_renderer->SetCursorChangeHandler([](CursorShape a_shape) {
				HardwareCursor::SetShape(a_shape);
			});
		}

		// Compositor
		_compositor = CreateCompositor();
		if (!_compositor->Initialize()) {
			REX::WARN("Runtime: compositor '{}' failed to initialize; falling back to null compositor", _compositor->Name());
			_compositor = std::make_unique<NullCompositor>();
			_compositor->Initialize();
		}
		// Size the view to the real output once the compositor knows it, so
		// the page renders aspect-correct instead of stretched.
		_compositor->SetOutputResizeCallback([this](std::uint32_t a_w, std::uint32_t a_h) { OnOutputResized(a_w, a_h); });
		REX::INFO("Runtime: compositor = {}", _compositor->Name());

		_captureInput.store(_config.captureInput);

		// Feature modules ("apps" on the platform). Core hosts them via the
		// IUiModule contract and knows nothing of what they do — settings is
		// just the first. This is the composition root; everything past here
		// treats modules generically. OnStart() applies persisted state (e.g.
		// fires the cursor-speed reaction) before the first frame.
		BuildModules();
		for (const auto& module : _modules) {
			module->OnStart();
		}

		// Views + bridge. The bridge and web->native handler are wired BEFORE
		// LoadView so no early page message can race past them; the renderer
		// queues native->web messages per view until each page is ready.
		if (view) {
			// The layer set to load, and which of those request the bridge.
			std::vector<std::string> toLoad = _config.views;
			if (toLoad.empty()) {
				toLoad.push_back(_config.view);
			}
			std::vector<std::string> bridgeViews;
			for (const auto& id : toLoad) {
				if (const auto* m = _views.Find(id); m && m->permissions.nativeBridge) {
					bridgeViews.push_back(id);
				}
			}

			// One feature-agnostic bridge serves every bridge-enabled view: the
			// renderer tags each inbound message with its source view, and replies
			// route back to that view (MessageBridge tracks the current source).
			if (!bridgeViews.empty()) {
				_bridge = std::make_unique<MessageBridge>([this](std::string_view a_viewId, std::string_view a_json) {
					if (_renderer) {
						_renderer->SendMessageToWeb(a_viewId, a_json);
					}
				});
				// Platform (window) commands live in core; everything else is a
				// module's to register.
				RegisterPlatformCommands(*_bridge);
				for (const auto& module : _modules) {
					module->RegisterCommands(*_bridge);
				}
				_renderer->SetWebMessageHandler([this](std::string_view a_viewId, std::string_view a_json) {
					if (_bridge) {
						_bridge->HandleWebMessage(a_viewId, a_json);
					}
				});
			} else {
				REX::INFO("Runtime: no loaded view requests nativeBridge; bridge disabled");
			}

			// Load the layer set and register each as a surface. Ordering, focus, and visibility are then owned by the MenuController + ApplyMenuPolicy, not the raw manifest order or a single active view.
			std::size_t loaded = 0;
			for (const auto& id : toLoad) {
				if (const auto* m = _views.Find(id)) {
					_renderer->LoadView(*m);
					_menus.Register({ id, m->kind, m->capturesInput, m->pausesGame, m->order });
					if (m->openOnStart) {
						_menus.Open(id);
					}
					++loaded;
				} else {
					REX::WARN("Runtime: configured view '{}' not found; skipping", id);
				}
			}
			REX::INFO("Runtime: loaded {} view(s); default menu = '{}'", loaded, _config.view);
			if (!_menus.IsRegistered(_config.view)) {
				REX::WARN("Runtime: default view '{}' is not among the loaded surfaces; the toggle key will have nothing to open (check config.view is listed in config.views)", _config.view);
			}

			// Greet each bridge-enabled view. The renderer queues this per view until that view's DOM is ready, so order here doesn't matter.
			if (_bridge) {
				for (const auto& id : bridgeViews) {
					_bridge->SendRuntimeReady(id);
				}
			}
		} else {
			REX::WARN("Runtime: configured view '{}' was not found; overlay has no content", _config.view);
		}

		// Hand the (possibly null) bridge to the native plugin API so a sibling
		// SFSE plugin's registered commands + queued sends apply on the next tick.
		// Null when no nativeBridge view loaded — the API then stays not-ready.
		// See docs/native-plugin-api.md.
		API::BridgeApi::Get().OnBridgeReady(_bridge.get());

		// Input. Key events reach the router from the WndProc subclass
		// (OverlayInputHook → OnHostKey), installed when config
		// inputSource="ui" (core/Plugin.cpp, kPostPostDataLoad).
		_toggleKey = ResolveKeyName(_config.toggleKey);
		if (_toggleKey != kInvalidKeyCode) {
			REX::INFO("Runtime: toggleKey '{}' resolved to VK code {:#x}", _config.toggleKey, _toggleKey);
		}
		_consoleKey = ResolveKeyName(_config.consoleKey);
		if (_consoleKey != kInvalidKeyCode) {
			REX::INFO("Runtime: consoleKey '{}' resolved to VK code {:#x} (passed through to the game so the console opens while the overlay is up)", _config.consoleKey, _consoleKey);
		}

		// F10 toggles the default menu; Esc (while captured) closes the top menu.
		_input.Configure(
			_toggleKey,
			[this] { EnqueueMenuRequest(MenuReq::ToggleDefault); },
			[this] { EnqueueMenuRequest(MenuReq::CloseTop); });

		// Keyboard routing into the web view, gated by capture state (Phase 4).
		_input.SetWebRouting(
			[this] { return IsInputCaptured(); },
			[this](KeyCode a_key, bool a_down) {
				if (_renderer) {
					_renderer->InjectKeyEvent(a_key, a_down);
				}
			});
		REX::INFO("Runtime: input capture {} (config captureInput)", _config.captureInput ? "enabled" : "disabled");

		_initialized = true;
		// Derive + push the initial policy (hidden/order/active/capture/visibility) from whatever is open. Also covers the closed case (nothing visible).
		ApplyMenuPolicy();
		REX::INFO("Runtime: initialized (visible={})", _visible.load());

		return true;
	}

	void Runtime::Shutdown()
	{
		// NOTE: SFSE provides no plugin shutdown callback; this is only ever
		// reached if we someday wire process-detach or an explicit teardown.
		// Everything here must stay safe to skip entirely.
		if (!_initialized) {
			return;
		}
		if (_compositor) {
			_compositor->Shutdown();
			_compositor.reset();
		}
		if (_renderer) {
			_renderer->Shutdown();
			_renderer.reset();
		}
		// Detach the native plugin API from the bridge before we destroy it, so its non-owning pointer never dangles and it reports not-ready.
		API::BridgeApi::Get().OnBridgeReady(nullptr);
		// Bridge before modules: its command handlers capture module pointers,
		// so it must not outlive them.
		_bridge.reset();
		_modules.clear();
		_initialized = false;
		REX::INFO("Runtime: shutdown complete");
	}

	void Runtime::Tick(double a_deltaSeconds)
	{
		if (!_initialized) {
			return;
		}
		_uptime += a_deltaSeconds;
		// Apply queued menu requests (F10/Esc/transition) on the MAIN thread first, so the reconcilers below and the frame submitted this tick reflect the new state.
		DrainMenuRequests();
		// Apply the native plugin API's queued ops (command (re)registration +
		// off-thread SendToWeb) on the main thread, before Update() flushes the
		// per-view outbound queues to the pages.
		API::BridgeApi::Get().PumpMainThread();
		// Reconcile engine menu-mode + control-disable toward the derived CAPTURE state (not visibility): a live HUD must not disable controls.
		if (_config.focusMenu) {
			ReconcileFocusMenu();
		}
		if (_config.disableControls) {
			ReconcileControlLayer();
		}
		if (!_renderer) {
			return;
		}
		// Fire any due crash-recovery reloads before Update pumps the renderer.
		DriveRecovery();
		_renderer->Update(a_deltaSeconds);
		SubmitFrameIfVisible();
	}

	void Runtime::EnqueueMenuRequest(MenuReq a_req)
	{
		// Callable from any thread (WndProc F10/Esc, MenuEventSink transition).
		// Leaf lock: it only guards the queue; the request is acted on in Tick.
		std::lock_guard lock(_reqMutex);
		_reqs.push_back(a_req);
	}

	void Runtime::DrainMenuRequests()
	{
		// Snapshot under the lock, then act unlocked, the actions call into the renderer/compositor and must never run while holding _reqMutex.
		std::vector<MenuReq> reqs;
		{
			std::lock_guard lock(_reqMutex);
			reqs.swap(_reqs);
		}
		// Menu opens/closes a sibling plugin requested by id via the bridge API (e.g. an
		// in-game item opening the scene browser). Same policy path as the F10 toggle below.
		auto pluginReqs = API::BridgeApi::Get().TakeMenuRequests();
		if (reqs.empty() && pluginReqs.empty()) {
			return;
		}
		for (const auto req : reqs) {
			switch (req) {
			case MenuReq::ToggleDefault:
				_menus.ToggleDefault(_config.view);
				break;
			case MenuReq::CloseTop:
				_menus.CloseTop();
				break;
			case MenuReq::CloseAll:
				_menus.CloseAll();
				break;
			}
		}
		for (const auto& r : pluginReqs) {
			if (r.open) {
				if (!_menus.Open(r.view)) {
					REX::WARN("Runtime: plugin RequestMenu('{}', open) ignored — not a registered surface (or already open)", r.view);
				}
			} else {
				_menus.Close(r.view);
			}
		}
		ApplyMenuPolicy();
	}

	void Runtime::ApplyMenuPolicy()
	{
		if (!_renderer) {
			return;
		}
		// Per-surface hidden + composite z (derived band order; the raw manifest zorder never governs menu/HUD paint order).
		for (const auto& layer : _menus.DesiredLayers()) {
			_renderer->SetViewHidden(layer.id, layer.hidden);
			_renderer->SetViewOrder(layer.id, layer.z);
		}
		// Focus follows the top menu; HUD-only => no active view to set.
		const auto active = _menus.ActiveMenu();
		if (active) {
			_renderer->SetActiveView(*active);
		}
		// Capture is the top menu's policy (false for HUD-only => the game keeps input).
		// This is the runtime writer of _captureInput that IsInputCaptured() reads; OnHost* handlers are unchanged.
		_captureInput.store(_menus.DesiredCapture());

		// Visibility side-effects are owned here rather than routed through a change-guarded helper (which would drop the compositor push on the no-change startup path).
		const bool visible = _menus.DesiredVisible();
		const bool wasVisible = _visible.exchange(visible);
		if (_compositor) {
			_compositor->SetVisible(visible);
		}

		// Recenter the virtual cursor on the closed->open edge; otherwise keep its position.
		// Either way, (re)place it in the active menu so a freshly focused  view shows the cursor at the right spot rather than at its stale origin.
		if (visible) {
			if (!wasVisible) {
				_cursorX = _viewWidth.load() * 0.5f;
				_cursorY = _viewHeight.load() * 0.5f;
			}
			if (active) {
				_renderer->InjectMouseMove(static_cast<int>(_cursorX), static_cast<int>(_cursorY));
				// Tell the newly-focused view it is being shown so it can play its
				// entry treatment (e.g. a dim-backdrop fade — "you're in a menu").
				// Only on the closed->open edge. The compositor hides same-frame on
				// close, so a fade-OUT can't render and is intentionally not
				// signalled (a real fade-out needs a close handshake — see
				// docs/menu-hud-framework-plan.md). _bridge may be null very early.
				if (!wasVisible && _bridge) {
					_bridge->SendToWeb(*active, "ui.visibility", nlohmann::json{ { "visible", true } });
				}
			}
		}
		if (visible != wasVisible) {
			REX::INFO("Runtime: overlay visibility -> {} (capture={})", visible, _captureInput.load());
		}
	}

	bool Runtime::IsVisible() const
	{
		return _visible.load();
	}

	bool Runtime::SetViewHidden(std::string_view a_id, bool a_hidden)
	{
		// Only views that were actually loaded can be addressed. The renderer
		// would silently no-op an unknown id, so reject here for a clear log.
		const auto& loaded = _config.views;
		const bool known = std::ranges::find(loaded, a_id) != loaded.end() ||
			(loaded.empty() && a_id == _config.view);
		if (!known) {
			REX::WARN("Runtime: setViewHidden ignored — '{}' is not a loaded view", a_id);
			return false;
		}
		if (_renderer) {
			_renderer->SetViewHidden(a_id, a_hidden);
		}
		REX::INFO("Runtime: view '{}' hidden -> {}", a_id, a_hidden);
		return true;
	}

	void Runtime::OnViewLoad(std::string_view a_viewId, bool a_failed, std::string_view a_url,
		std::string_view a_description, int a_errorCode)
	{
		const std::string id(a_viewId);
		_viewLoadState[id] = a_failed ? ViewLoadState::Failed : ViewLoadState::Finished;
		if (!a_failed) {
			// A healthy load clears the view's strikes, so a much later failure
			// gets the full retry budget again.
			if (_recovery.erase(id) > 0) {
				REX::INFO("Runtime: view '{}' recovered ({})", a_viewId, a_url);
			} else {
				REX::INFO("Runtime: view '{}' finished loading ({})", a_viewId, a_url);
			}
			return;
		}

		REX::ERROR("Runtime: view '{}' FAILED to load ({}): {} [{}]",
			a_viewId, a_url, a_description, a_errorCode);

		// URL crash-recovery: schedule a bounded reload with backoff. attempts
		// counts reloads already fired; the budget exhausted means the content
		// is genuinely broken — tear the view down and unregister its surface so
		// F10/menu.open cannot re-open an invisible, input-capturing shell.
		constexpr std::uint32_t kMaxAttempts = 3;
		constexpr double        kBackoffSec[kMaxAttempts] = { 2.0, 5.0, 15.0 };
		auto& rec = _recovery[id];
		if (rec.attempts >= kMaxAttempts) {
			REX::ERROR("Runtime: view '{}' still failing after {} reload attempts; giving up — "
					   "destroying the view and removing its surface (fix the view's files and relaunch)",
				a_viewId, rec.attempts);
			_recovery.erase(id);
			if (_renderer) {
				_renderer->DestroyView(id);
			}
			if (_menus.Unregister(id)) {
				ApplyMenuPolicy();  // it was open: release capture/visibility now
			}
			return;
		}
		rec.pending = true;
		rec.retryAt = _uptime + kBackoffSec[rec.attempts];
		REX::WARN("Runtime: view '{}' reload attempt {}/{} scheduled in {:.0f}s",
			a_viewId, rec.attempts + 1, kMaxAttempts, kBackoffSec[rec.attempts]);
	}

	void Runtime::DriveRecovery()
	{
		if (_recovery.empty() || !_renderer) {
			return;
		}
		for (auto& [id, rec] : _recovery) {
			if (!rec.pending || _uptime < rec.retryAt) {
				continue;
			}
			rec.pending = false;
			const auto* manifest = _views.Find(id);
			if (!manifest) {
				continue;  // shouldn't happen: only loaded (known) views get load events
			}
			++rec.attempts;
			REX::INFO("Runtime: crash-recovery reloading view '{}' (attempt {})", id, rec.attempts);
			_renderer->LoadView(*manifest);
			// A recreated view starts at manifest dimensions; restore the
			// output-matched size so it composites 1:1 again.
			_renderer->Resize(_viewWidth.load(), _viewHeight.load());
		}
	}

	Runtime::ViewLoadState Runtime::GetViewLoadState(std::string_view a_id) const
	{
		const auto it = _viewLoadState.find(std::string(a_id));
		return it == _viewLoadState.end() ? ViewLoadState::Loading : it->second;
	}

	bool Runtime::IsInputCaptured() const
	{
		return _initialized && _captureInput.load() && _visible.load();
	}

	bool Runtime::OnHostKey(std::uint32_t a_vkCode, bool a_down)
	{
		// Console key: the game toggles its console from this key via MenuControls,
		// which OSF RE (module ui.menu_input) proved runs BEFORE any per-menu UI
		// input dispatch — so nothing on the menu stack can starve it, and the ONLY
		// thing stopping the console while the overlay is up is our own WndProc
		// swallow. Never consume it and never route it into the web view: hand it to
		// the game untouched (both edges — the toggle fires on release). On the down
		// edge, if the overlay is capturing, dismiss it so the console (which we
		// composite over at Present) isn't left hidden behind the overlay. When the
		// overlay is already closed this is a plain pass-through, matching gameplay.
		if (_consoleKey != kInvalidKeyCode && a_vkCode == _consoleKey) {
			if (a_down && IsInputCaptured()) {
				EnqueueMenuRequest(MenuReq::CloseAll);
			}
			return false;
		}

		// Decide consumption before routing: capturing OR the toggle key must
		// not reach the game. (The toggle press itself is captured so opening
		// the overlay never also acts in-game.)
		const bool consume = IsInputCaptured() || a_vkCode == _toggleKey;
		if (a_down) {
			_input.OnKeyDown(a_vkCode);
		} else {
			_input.OnKeyUp(a_vkCode);
		}
		return consume;
	}

	void Runtime::OnHostChar(std::uint32_t a_codepoint)
	{
		if (!IsInputCaptured() || !_renderer) {
			return;
		}
		// Pure text entry — no toggle/focus logic, so route straight to the
		// active view (the VK stream handles toggle/focus via OnHostKey).
		_renderer->InjectCharEvent(a_codepoint);
	}

	void Runtime::OnHostMouseAbsolute(int a_clientX, int a_clientY, int a_clientW, int a_clientH)
	{
		if (!IsInputCaptured() || !_renderer || a_clientW <= 0 || a_clientH <= 0) {
			return;
		}
		// The OS pointer moves in window-client space; the view is the same
		// aspect but height-capped (OnOutputResized), so scale through the
		// client size. Uniform scale => the pointer and the page's hit-testing
		// stay aligned at every resolution.
		const auto viewW = static_cast<float>(_viewWidth.load(std::memory_order_relaxed));
		const auto viewH = static_cast<float>(_viewHeight.load(std::memory_order_relaxed));
		_cursorX = std::clamp(static_cast<float>(a_clientX) * viewW / static_cast<float>(a_clientW), 0.0f, viewW - 1.0f);
		_cursorY = std::clamp(static_cast<float>(a_clientY) * viewH / static_cast<float>(a_clientH), 0.0f, viewH - 1.0f);
		_renderer->InjectMouseMove(static_cast<int>(_cursorX), static_cast<int>(_cursorY));
	}

	void Runtime::OnHostMouseDelta(int a_dx, int a_dy)
	{
		if (!IsInputCaptured() || !_renderer) {
			return;
		}
		// Scale raw deltas so the cursor crosses the view in a screen-size-
		// independent amount of physical mouse travel (the view tracks the
		// screen now, so a fixed 1:1 mapping would feel slow on big views),
		// times the user's live cursor-speed setting (osfui.cursorSpeed).
		const auto scale = _cursorScale.load(std::memory_order_relaxed) *
			_cursorSpeed.load(std::memory_order_relaxed);
		const auto maxX = static_cast<float>(_viewWidth.load(std::memory_order_relaxed) - 1);
		const auto maxY = static_cast<float>(_viewHeight.load(std::memory_order_relaxed) - 1);
		_cursorX = std::clamp(_cursorX + static_cast<float>(a_dx) * scale, 0.0f, maxX);
		_cursorY = std::clamp(_cursorY + static_cast<float>(a_dy) * scale, 0.0f, maxY);
		_renderer->InjectMouseMove(static_cast<int>(_cursorX), static_cast<int>(_cursorY));
	}

	void Runtime::OnHostMouseButton(int a_button, bool a_down)
	{
		if (!IsInputCaptured() || !_renderer) {
			return;
		}
		_renderer->InjectMouseButton(static_cast<int>(_cursorX), static_cast<int>(_cursorY), a_button, a_down);
	}

	void Runtime::OnHostMouseWheel(int a_wheelDelta)
	{
		if (!IsInputCaptured() || !_renderer) {
			return;
		}
		// Route at the current virtual cursor; the renderer converts notches to
		// pixels via the per-view scroll step (SetScrollPixelSize).
		_renderer->InjectMouseWheel(static_cast<int>(_cursorX), static_cast<int>(_cursorY), a_wheelDelta);
	}

	void Runtime::ReconcileFocusMenu()
	{
		// Runs on the game main thread (Tick). Drive the engine menu's open state
		// toward the top menu's CAPTURE policy, and its kPausesGame flag toward
		// the top menu's PAUSE policy (manifest pausesGame — Step 3, finally live
		// now that Route A admits the menu). The engine recomputes the pause
		// latch on menu open/close events, not on flag writes to an open menu, so
		// a pause-policy change while open closes the menu this tick and lets the
		// next tick reopen it with the new flag (also avoids stacking kHide+kShow
		// in one queue pump). Only act on a change — no per-frame queue spam.
		const bool wantOpen = _menus.DesiredCapture();
		const bool wantPause = _menus.DesiredPause();

		if (_focusMenuOpen && (!wantOpen || wantPause != _focusMenuPause)) {
			FocusMenu::Close();
			_focusMenuOpen = false;
			return;
		}
		if (!_focusMenuOpen && wantOpen) {
			FocusMenu::SetPausesGame(wantPause);  // BEFORE Open: the open-path recompute reads it
			_focusMenuPause = wantPause;
			FocusMenu::Open();
			_focusMenuOpen = true;
		}
	}

	void Runtime::ReconcileControlLayer()
	{
		// Main-thread (Tick). Drive the input-enable layer toward the top menu's CAPTURE policy; this is the ONLY gate that stops gamepad/XInput,
		// so it must track capture (not pause), or a gamepad drives the game underneath a capturing menu.
		// A live HUD (no capture) leaves controls enabled. Engage() may no-op until gameplay (manager not ready at the main menu); IsEngaged() stays false then, so we simply retry next tick.
		const bool wantEngaged = _menus.DesiredCapture();
		if (wantEngaged == ControlLayer::IsEngaged()) {
			return;
		}
		if (wantEngaged) {
			ControlLayer::Engage();
		} else {
			ControlLayer::Release();
		}
	}

	void Runtime::BuildModules()
	{
		// Settings: schemas ship read-only under <data>/settings/*.json; values
		// persist per-mod to a writable dir (Documents — NOT the MO2/
		// Program-Files-mapped data dir). The change listener is how core reacts
		// to settings it owns; the module itself is feature-agnostic.
		const auto schemaDir = Paths::DataDir() / "settings";
		const auto docs = Platform::GetDocumentsPath();
		const auto valuesDir = docs.empty()
			? Paths::DataDir() / "settings" / "values"  // fallback (MO2 redirects the write)
			: docs / "My Games" / "Starfield" / "OSFUI" / "settings";
		_modules.push_back(std::make_unique<SettingsModule>(schemaDir, valuesDir,
			[this](std::string_view a_mod, std::string_view a_key, const nlohmann::json& a_value) {
				OnSettingChanged(a_mod, a_key, a_value);
			}));

		REX::INFO("Runtime: {} UI module(s) loaded", _modules.size());
	}

	void Runtime::RegisterPlatformCommands(MessageBridge& a_bridge)
	{
		// The platform owns only window/diagnostic commands. Features register
		// their own; there is no generic "call native" escape hatch.
		a_bridge.RegisterCommand("close", [this](const nlohmann::json&, MessageBridge& a_b) {
			// Dismiss the calling surface. Closing the last open menu empties the stack, so the overlay hides; same effect as the old global close, but a coexisting live HUD (if any) stays up by design.
			if (_menus.Close(a_b.CurrentSource())) {
				ApplyMenuPolicy();
			}
		});
		a_bridge.RegisterCommand("setVisible", [this](const nlohmann::json& a_p, MessageBridge& a_b) {
			const std::string src(a_b.CurrentSource());
			const bool changed = Json::GetBool(a_p, "visible", false) ? _menus.Open(src) : _menus.Close(src);
			if (changed) {
				ApplyMenuPolicy();
			}
		});
		// Open/close a surface by id (defaults to the calling view). menu.* and hud.* are aliases — a surface's kind is fixed by its manifest, not the command used.
		const auto surfaceOpen = [this](const nlohmann::json& a_p, MessageBridge& a_b) {
			std::string id = Json::GetString(a_p, "view", "");
			if (id.empty()) {
				id = std::string(a_b.CurrentSource());
			}
			if (_menus.Open(id)) {
				ApplyMenuPolicy();
			} else {
				REX::WARN("Runtime: menu.open/hud.show ignored — '{}' is not a registered surface (or already open)", id);
			}
		};
		const auto surfaceClose = [this](const nlohmann::json& a_p, MessageBridge& a_b) {
			std::string id = Json::GetString(a_p, "view", "");
			if (id.empty()) {
				id = std::string(a_b.CurrentSource());
			}
			if (_menus.Close(id)) {
				ApplyMenuPolicy();
			}
		};
		a_bridge.RegisterCommand("menu.open", surfaceOpen);
		a_bridge.RegisterCommand("menu.close", surfaceClose);
		a_bridge.RegisterCommand("hud.show", surfaceOpen);
		a_bridge.RegisterCommand("hud.hide", surfaceClose);
		a_bridge.RegisterCommand("setViewHidden", [this](const nlohmann::json& a_p, MessageBridge& a_b) {
			// Show/hide one loaded view by id, independent of the overlay toggle.
			// Omitting "view" targets the calling view (self-hide).
			std::string id = Json::GetString(a_p, "view", "");
			if (id.empty()) {
				id = std::string(a_b.CurrentSource());
			}
			SetViewHidden(id, Json::GetBool(a_p, "hidden", false));
		});
		a_bridge.RegisterCommand("log", [](const nlohmann::json& a_p, MessageBridge&) {
			// Untrusted content: bound the length so JS cannot flood the log.
			REX::INFO("MessageBridge: [web] {}", Json::GetString(a_p, "text", "").substr(0, 512));
		});
		a_bridge.RegisterCommand("ping", [](const nlohmann::json&, MessageBridge& a_b) {
			a_b.SendToWeb("runtime.pong", nlohmann::json::object());
		});

		// First read-only game-data provider: the in-game calendar (date/time).
		// Bridge handlers dispatch from Tick()/Update() on the game's Main thread,
		// so reading game singletons here is safe. Calendar is null before a save
		// is loaded (main menu) — reported as available:false. A view polls this
		// (e.g. once a second) and renders the result. Likely graduates to its own
		// IUiModule as game-data grows (architecture.md "Feature modules").
		a_bridge.RegisterCommand("game.get", [](const nlohmann::json&, MessageBridge& a_b) {
			nlohmann::json data = nlohmann::json::object();
			if (const auto* cal = RE::Calendar::GetSingleton()) {
				data["available"] = true;
				data["day"] = cal->GetDay();
				data["month"] = cal->GetMonth();
				data["year"] = cal->GetYear();
				data["hour"] = cal->GetHour();
				data["daysPassed"] = cal->GetDaysPassedExact();
			} else {
				data["available"] = false;
			}
			a_b.SendToWeb("game.data", data);
		});
	}

	void Runtime::OnSettingChanged(std::string_view a_modId, std::string_view a_key, const nlohmann::json& a_value)
	{
		// The Phase 5b payoff: settings drive native behaviour live. Cursor
		// speed multiplies mouse sensitivity in OnHostMouseDelta — observable
		// immediately and never self-defeating (the cursor keeps working).
		if (a_modId == "osfui" && a_key == "cursorSpeed" && a_value.is_number()) {
			const auto speed = a_value.get<float>();
			_cursorSpeed.store(speed);
			REX::INFO("Runtime: setting osfui.cursorSpeed -> {:.2f}", speed);
		}
	}

	void Runtime::OnOutputResized(std::uint32_t a_width, std::uint32_t a_height)
	{
		if (a_width == 0 || a_height == 0 || !_renderer) {
			return;
		}
		// Match the view's aspect to the screen, capped to a sane UI height so
		// CPU rasterization stays bounded on 4K+ (the page is responsive, so
		// any size lays out correctly). Equal aspect => the compositor's
		// fill-the-backbuffer draw is a uniform scale, i.e. no distortion.
		constexpr std::uint32_t kMaxViewHeight = 1440;
		const auto viewHeight = (std::min)(a_height, kMaxViewHeight);
		const auto viewWidth = static_cast<std::uint32_t>(
			std::lround(static_cast<double>(a_width) * viewHeight / a_height));

		if (viewWidth == _viewWidth.load() && viewHeight == _viewHeight.load()) {
			return;
		}

		_viewWidth.store(viewWidth);
		_viewHeight.store(viewHeight);
		// Keep cursor speed consistent across resolutions: ~1920 counts to
		// sweep the view width, regardless of view size.
		_cursorScale.store((std::max)(1.0f, static_cast<float>(viewWidth) / 1920.0f));
		_renderer->Resize(viewWidth, viewHeight);
		REX::INFO("Runtime: output {}x{} -> view resized to {}x{} (aspect-correct)",
			a_width, a_height, viewWidth, viewHeight);
	}

	void Runtime::SubmitFrameIfVisible()
	{
		if (!_initialized || !IsVisible() || !_renderer || !_compositor) {
			return;
		}
		if (const auto frame = _renderer->Render()) {
			_compositor->Submit(*frame);
		}
	}

	std::unique_ptr<IWebRenderer> Runtime::CreateRenderer() const
	{
		if (_config.renderer == "null") {
			return std::make_unique<NullWebRenderer>();
		}
		if (_config.renderer == "mock") {
			return std::make_unique<MockWebRenderer>();
		}
		if (_config.renderer == "ultralight") {
#if defined(OSFUI_WITH_ULTRALIGHT)
			// Must run before the renderer object exists: even constructing
			// it touches delay-loaded SDK symbols (see PreloadRuntime docs).
			if (!UltralightWebRenderer::PreloadRuntime(Paths::DataDir())) {
				REX::ERROR("Runtime: Ultralight runtime preload failed; using null renderer");
				return std::make_unique<NullWebRenderer>();
			}
			return std::make_unique<UltralightWebRenderer>();
#else
			REX::WARN("Runtime: renderer 'ultralight' requested but this build was compiled without "
					  "with_ultralight; using null renderer");
			return std::make_unique<NullWebRenderer>();
#endif
		}
		REX::WARN("Runtime: unknown renderer '{}'; using null renderer", _config.renderer);
		return std::make_unique<NullWebRenderer>();
	}

	std::unique_ptr<ICompositor> Runtime::CreateCompositor() const
	{
		if (_config.compositor == "d3d12") {
			// Uploads frames to a GPU texture on the game's device (located
			// lazily; see composite/EngineD3D12.h) and draws the overlay at
			// present time via a Present slot-8 vtable hook.
			return std::make_unique<D3D12Compositor>();
		}
		if (_config.compositor != "null") {
			REX::WARN("Runtime: unknown compositor '{}'; using null compositor", _config.compositor);
		}
		return std::make_unique<NullCompositor>();
	}

}
