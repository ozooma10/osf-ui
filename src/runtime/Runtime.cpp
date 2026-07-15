#include "runtime/Runtime.h"

#include <cmath>

#include "RE/C/Calendar.h"

#include "api/BridgeApi.h"
#include "composite/D3D12Compositor.h"
#include "composite/NullCompositor.h"
#include "core/Log.h"
#include "input/ControlLayer.h"
#include "input/EngineInput.h"
#include "input/FocusMenu.h"
#include "input/FreeCursor.h"
#include "input/HardwareCursor.h"
#include "input/PauseMenuEntry.h"
#include "input/SimPause.h"
#include "core/Paths.h"
#include "platform/WindowsPlatform.h"
#include "render/MockWebRenderer.h"
#include "runtime/Json.h"
#include "runtime/VanillaKeys.h"
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

		// The injected PauseMenu entry's label + target view (Reconcile itself
		// is gated on config.pauseMenuEntry in Tick).
		PauseMenuEntry::Configure(_config.pauseMenuEntryLabel, _config.pauseMenuEntryView);

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
		// hardwareCursor is a boot-time config knob (config.json), not a runtime
		// setting — its only alternative is an invisible software cursor (debug
		// escape hatch), so it's deliberately not surfaced in the settings UI.
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
					// The policy fields decide runtime behavior (capture, sim pause)
					// and an explicit manifest value silently overrides the defaults
					// — log them so a "why doesn't it pause" is a log-read away.
					REX::INFO("Runtime: surface '{}' registered ({}, capturesInput={}, pausesGame={})",
						id, m->kind == SurfaceKind::Hud ? "hud" : "menu", m->capturesInput, m->pausesGame);
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
		// Dev view-reload key (mcm-design.md §12.1): resolved only in devMode
		// — kInvalid is the whole gate in OnHostKey, so a user config with the
		// shipped devReloadKey but devMode off never loses the key to us.
		if (_config.devMode && !_config.devReloadKey.empty()) {
			_devReloadKey = ResolveKeyName(_config.devReloadKey);
			if (_devReloadKey != kInvalidKeyCode) {
				REX::INFO("Runtime: devReloadKey '{}' resolved to VK code {:#x} (reloads the top open menu)", _config.devReloadKey, _devReloadKey);
			}
		}

		EngineInput::SetEnabled(_config.engineInput);
		if (_config.engineInput) {
			REX::INFO("Runtime: engineInput enabled — engine per-menu input (gamepad) routed into the focused view; keyboard/mouse stay on the WndProc path");
		}

		// F10 toggles the default menu; Esc (while captured) closes the top menu.
		// Extracted so a live key-rebind (osfui.toggleKey) can re-apply it.
		ApplyToggleKey();

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
		// Same for modules that retain the bridge for unsolicited pushes.
		for (const auto& module : _modules) {
			module->OnBridgeDown();
		}
		// Bridge before modules: its command handlers capture module pointers,
		// so it must not outlive them.
		_bridge.reset();
		_settings = nullptr;  // owned by _modules, about to go away
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
		// config.pauseMenuEntry: keep the injected "mod settings" entry present
		// in the engine PauseMenu and act on its clicks. BEFORE the snapshot
		// below so a click's EnqueueOpenView lands
		// this same tick (kHide for the pause menu is queued inside; the
		// overlay open then applies through the normal policy path).
		if (_config.pauseMenuEntry) {
			PauseMenuEntry::Reconcile();
		}
		// SNAPSHOT queued menu requests (F10/Esc/transition + plugin RequestMenu)
		// now, but APPLY them after the bridge pump below — the ABI 1.3 ordering
		// guarantee: a consumer that called SendToWeb(v, ...) and then
		// RequestMenu(v, true) has its send in _pendingSends before the request
		// entered this snapshot, so the pump flushes the message into v's queue
		// before the open unhides v (message before first visible paint).
		const auto menuWork = TakeMenuRequests();
		// Deliver a captured rebind key back to the settings view (main thread).
		DrainKeyCapture();
		// Deliver queued hotkey fires (window thread -> main, mcm-design.md
		// §9) BEFORE the bridge pump below, so the C ABI callbacks they queue
		// are invoked this same tick.
		DrainHotkeys();
		// Apply queued runtime schema (un)registrations to the store first,
		// so their value replay is already queued when the pump below drains
		// SubscribeSettings callbacks — registration lands in one tick.
		DrainSchemaOps();
		// Apply the native plugin API's queued ops (command (re)registration +
		// off-thread SendToWeb) on the main thread, before Update() flushes the
		// per-view outbound queues to the pages.
		API::BridgeApi::Get().PumpMainThread();
		// NOW apply the snapshot, so the reconcilers below and the frame
		// submitted this tick reflect the new menu state.
		ApplyMenuRequests(menuWork);
		// Land coalesced settings value writes once their write-behind window
		// elapses (mcm-design.md §8.1) — a slider drag costs one disk write
		// per ~500ms, not one per step.
		if (_settings) {
			_settings->Store().PumpPersistence(_uptime);
			// Schema hot-reload (mcm-design.md §12.1, devMode): edited
			// settings/*.json files reload live, values preserved; the
			// registry re-broadcast repaints any open settings view.
			if (_config.devMode) {
				_settings->PumpSchemaHotReload(_uptime);
			}
		}
		// Reconcile engine menu-mode + control-disable toward the derived CAPTURE state (not visibility): a live HUD must not disable controls.
		if (_config.focusMenu) {
			ReconcileFocusMenu();
		}
		// Always reconcile: the config.disableControls check lives INSIDE, so a
		// config with it off still RELEASES any engaged lock (a gate here would
		// just stop reconciling and strand the player's controls).
		ReconcileControlLayer();
		// Sim pause (manifest pausesGame) — unconditional: it is a direct
		// Main::isGameMenuPaused write, independent of the engine focus menu.
		ReconcileSimPause();
		// OS-cursor release — unconditional, tracks CAPTURE (the same policy
		// that activates the hardware cursor): while a menu captures input, hold
		// a reference on MenuCursor::freeCursorRefCount so the per-frame clip
		// releases the pointer (no engine arrow — the focus menu carries no
		// ShowCursor bit). Edge-triggered inside Apply.
		FreeCursor::Apply(_menus.DesiredCapture());
		// Route engine-delivered gamepad input into the active view (Level 2,
		// increment 3). No-op unless config.engineInput.
		if (_config.engineInput) {
			DrainEngineInput(a_deltaSeconds);
		}
		if (!_renderer) {
			return;
		}
		// Fire any due crash-recovery reloads before Update pumps the renderer.
		DriveRecovery();
		// Dev view-reload keypress (devMode): reload the top open menu now.
		DriveDevReload();
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

	void Runtime::EnqueueOpenView(std::string a_viewId)
	{
		// Callable from any thread (PauseMenuEntry click, future native
		// triggers). Same leaf-lock discipline as EnqueueMenuRequest.
		std::lock_guard lock(_reqMutex);
		_openViewReqs.push_back(std::move(a_viewId));
	}

	Runtime::PendingMenuWork Runtime::TakeMenuRequests()
	{
		// Snapshot under the lock, then act unlocked (in ApplyMenuRequests) —
		// the actions call into the renderer/compositor and must never run
		// while holding _reqMutex.
		PendingMenuWork work;
		{
			std::lock_guard lock(_reqMutex);
			work.local.swap(_reqs);
			work.openViews.swap(_openViewReqs);
		}
		// Menu opens/closes a sibling plugin requested by id via the bridge API (e.g. an
		// in-game item opening the scene browser). Same policy path as the F10 toggle.
		work.plugin = API::BridgeApi::Get().TakeMenuRequests();
		return work;
	}

	void Runtime::ApplyMenuRequests(const PendingMenuWork& a_work)
	{
		const auto& reqs = a_work.local;
		const auto& pluginReqs = a_work.plugin;
		if (reqs.empty() && pluginReqs.empty() && a_work.openViews.empty()) {
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
		for (const auto& id : a_work.openViews) {
			if (!_menus.Open(id)) {
				REX::WARN("Runtime: EnqueueOpenView('{}') ignored — not a registered surface (or already open)", id);
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

	void Runtime::DrainSchemaOps()
	{
		if (!_settings) {
			return;  // no store yet — ops keep waiting in BridgeApi's queue
		}
		auto ops = API::BridgeApi::Get().TakeSchemaOps();
		if (ops.empty()) {
			return;
		}
		auto& store = _settings->Store();
		for (auto& op : ops) {
			if (!op.schema.is_null()) {
				// Shape was validated synchronously at the ABI boundary;
				// what's left here is precedence (native wins, logged inside).
				store.RegisterSchema(std::move(op.schema), SettingsStore::Source::kNative);
			} else if (store.GetSource(op.modId) == SettingsStore::Source::kNative) {
				store.RemoveMod(op.modId);
			} else {
				REX::WARN("Runtime: UnregisterSettingsSchema('{}') ignored — not a runtime-registered schema", op.modId);
			}
		}
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
			if (visible && !wasVisible) {
				// Closed->open edge: DEFER the reveal. The compositor redraws
				// its last cached texture every present while visible, so
				// showing it now would flash stale pre-open content for the
				// frames it takes the renderer to deliver queued messages and
				// hand over a post-open frame (see _revealPending).
				_revealPending = true;
			} else {
				if (!visible) {
					_revealPending = false;  // closed while a reveal was still pending
				}
				if (!_revealPending) {
					_compositor->SetVisible(visible);
				}
			}
		}

		// Open->closed edge: the user just finished editing — flush the
		// settings write-behind now instead of letting the window run out
		// (mcm-design.md §8.1: "flushed on menu close and shutdown"; shutdown
		// is ~SettingsStore).
		if (!visible && wasVisible && _settings) {
			_settings->Store().FlushPersistence();
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
				// Only on the closed->open edge. _bridge may be null very early.
				if (!wasVisible && _bridge) {
					_bridge->SendToWeb(*active, "ui.visibility", nlohmann::json{ { "visible", true } });
					_lastShownView = *active;
				}
			}
		} else if (wasVisible && _bridge && !_lastShownView.empty()) {
			// Open->closed edge: the compositor already hid the overlay this frame, so no fade-OUT
			// can render (a rendered fade-out would need a real close handshake — see
			// docs/menu-hud-framework-plan.md). The hide is still SIGNALLED: the view's JS keeps
			// running while hidden, and consumers need the edge (e.g. the OSF scene browser
			// forwards it so the orbit camera can switch between drag-steer and free-look).
			_bridge->SendToWeb(_lastShownView, "ui.visibility", nlohmann::json{ { "visible", false } });
			_lastShownView.clear();
		}
		if (visible != wasVisible) {
			REX::INFO("Runtime: overlay visibility -> {} (capture={})", visible, _captureInput.load());
		}

		// Push the surface catalog to hub-style subscribers (deduped: no-op when
		// nothing in the catalog actually changed).
		BroadcastViewsData();
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
			BroadcastViewsData();  // loadState loading -> loaded
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
			_viewsSubscribers.erase(id);  // a destroyed view can't receive pushes
			for (const auto& mod : _modules) {
				mod->OnViewDestroyed(id);  // module-held subscriber sets too (e.g. settings)
			}
			BroadcastViewsData();  // it also drops out of the catalog
			return;
		}
		rec.pending = true;
		rec.retryAt = _uptime + kBackoffSec[rec.attempts];
		REX::WARN("Runtime: view '{}' reload attempt {}/{} scheduled in {:.0f}s",
			a_viewId, rec.attempts + 1, kMaxAttempts, kBackoffSec[rec.attempts]);
		BroadcastViewsData();  // loadState -> failed
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

	void Runtime::DriveDevReload()
	{
		if (!_devReloadRequested.exchange(false) || !_renderer) {
			return;
		}
		// The top open menu is what the author is looking at. HUD-only setups
		// have no reload target here — dev iteration on HUDs goes through the
		// browser harness (mcm-design.md §12.2).
		const auto active = _menus.ActiveMenu();
		if (!active) {
			REX::INFO("Runtime: dev reload — no open menu to reload");
			return;
		}
		const auto* manifest = _views.Find(*active);
		if (!manifest) {
			return;
		}
		REX::INFO("Runtime: dev-reloading view '{}' (devReloadKey)", *active);
		// Same pair as crash-recovery: fresh URL load, then restore the
		// output-matched size so it composites 1:1 again. A load-state event
		// follows from the renderer (OnViewLoad), same as any load.
		_viewLoadState[*active] = ViewLoadState::Loading;
		BroadcastViewsData();
		_renderer->LoadView(*manifest);
		_renderer->Resize(_viewWidth.load(), _viewHeight.load());
	}

	nlohmann::json Runtime::BuildViewsData() const
	{
		nlohmann::json views = nlohmann::json::array();
		const auto     active = _menus.ActiveMenu();
		for (const auto& m : _views.All()) {
			// Discovered-but-not-loaded manifests (not in config.views) and views
			// torn down by crash-recovery are not registered — not part of the
			// catalog: menu.open on them would fail anyway.
			if (!_menus.IsRegistered(m.id)) {
				continue;
			}
			const auto state = GetViewLoadState(m.id);
			views.push_back(nlohmann::json{
				{ "id", m.id },
				{ "title", m.title },
				{ "description", m.description },
				{ "kind", m.kind == SurfaceKind::Hud ? "hud" : "menu" },
				{ "interactive", m.interactive },
				{ "hub", m.hub },
				{ "open", _menus.IsOpen(m.id) },
				{ "focused", active.has_value() && *active == m.id },
				{ "loadState", state == ViewLoadState::Failed ? "failed" :
				               state == ViewLoadState::Finished ? "loaded" :
				                                                  "loading" },
			});
		}
		return nlohmann::json{ { "views", std::move(views) } };
	}

	void Runtime::BroadcastViewsData()
	{
		if (!_bridge || _viewsSubscribers.empty()) {
			return;
		}
		auto payload = BuildViewsData();
		auto dumped = payload.dump();
		if (dumped == _lastViewsData) {
			return;
		}
		_lastViewsData = std::move(dumped);
		for (const auto& id : _viewsSubscribers) {
			_bridge->SendToWeb(id, "views.data", payload);
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

	void Runtime::ApplyToggleKey()
	{
		_input.Configure(
			_toggleKey,
			[this] { EnqueueMenuRequest(MenuReq::ToggleDefault); },
			[this] { EnqueueMenuRequest(MenuReq::CloseTop); });
	}

	bool Runtime::OnHostKey(std::uint32_t a_vkCode, bool a_down)
	{
		// Key-rebind capture (armed by the settings.captureKey command). Grab the
		// next key press for the rebind and CONSUME it, so pressing the current
		// toggle key (or Esc) rebinds instead of closing the overlay. The actual
		// apply happens on the main thread (DrainKeyCapture) — here we only stash
		// the VK. The matching key-up is swallowed too so it can't leak/route.
		if (_captureArmed.load()) {
			if (a_down) {
				_capturedVk.store(a_vkCode);
				_captureArmed.store(false);
				_captureUpVk = a_vkCode;
			}
			return true;
		}
		if (_captureUpVk != kInvalidKeyCode && a_vkCode == _captureUpVk && !a_down) {
			_captureUpVk = kInvalidKeyCode;
			return true;
		}

		// Dev view-reload key (mcm-design.md §12.1; _devReloadKey only resolves
		// in devMode). Window thread: just raise the flag — the reload runs
		// from Tick (DriveDevReload; renderer calls are main-thread). Consumed
		// on both edges like the toggle key, and BEFORE hotkey dispatch so a
		// mod binding the same key doesn't also fire. Capture (above) still
		// wins: mid-rebind this key is a binding like any other.
		if (_devReloadKey != kInvalidKeyCode && a_vkCode == _devReloadKey) {
			if (a_down) {
				_devReloadRequested.store(true);
			}
			return true;
		}

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

		// Hotkey dispatch (mcm-design.md §9): a key-DOWN edge may fire mods'
		// key-typed bindings. The service self-suppresses while the overlay
		// captures input or a rebind is armed (the armed path above returned
		// already — belt and braces); fires queue here on the window thread
		// and deliver from Tick (DrainHotkeys). Never consumes: the game (and
		// the toggle/router path below) still sees the key.
		if (a_down) {
			_hotkeys.OnKeyDown(a_vkCode);
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
		// screen now, so a fixed 1:1 mapping would feel slow on big views).
		const auto scale = _cursorScale.load(std::memory_order_relaxed);
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
		// toward the top menu's CAPTURE policy. Pause is deliberately NOT wired
		// through this menu's flags (the real pause flag, bit 1, would tie pause
		// to capture instead of the per-view pausesGame policy) — the sim pause
		// is ReconcileSimPause. Only act on a change — no per-frame queue spam.
		const bool wantOpen = _menus.DesiredCapture();
		if (wantOpen == _focusMenuOpen) {
			return;
		}
		_focusMenuOpen = wantOpen;
		if (wantOpen) {
			FocusMenu::Open();
		} else {
			FocusMenu::Close();
			// One observer summary per overlay session (no-op unless engineInput).
			EngineInput::LogSessionSummary();
			// Raw-passthrough is a per-session grant: without this, a page that
			// took the gamepad (osfui.gamepadRaw) would leave DEFAULT NAV DEAD
			// for whichever menu opens next. Pages re-assert on each
			// ui.visibility show.
			EngineInput::SetRawMode(false);
		}
	}

	void Runtime::ReconcileSimPause()
	{
		// Main thread (Tick), unconditional — the sim pause needs no engine menu
		// (UI::ModifyMenuPauseCounter; see input/SimPause), so it is not gated on
		// config.focusMenu. Driven by the top menu's manifest pausesGame (default
		// true for menus). Edge-triggered inside Apply.
		SimPause::Apply(_menus.DesiredPause());
	}

	void Runtime::DrainEngineInput(double a_deltaSeconds)
	{
		if (!_renderer) {
			return;
		}
		const bool captured = IsInputCaptured();
		const auto active = _menus.ActiveMenu();
		const bool raw = EngineInput::IsRawMode();

		// Discrete down+up tap: robust against a missed release (no stuck key).
		const auto tap = [this](std::uint32_t a_vk) {
			_renderer->InjectKeyEvent(a_vk, true);
			_renderer->InjectKeyEvent(a_vk, false);
		};

		// ---- button edges (queued by the worker-thread thunks) ----
		EngineInput::GamepadButtonEdge e;
		while (EngineInput::PollGamepadButton(e)) {
			if (!captured) {
				continue;  // drain-and-discard while not capturing
			}
			// Raw event for every edge — a page may own gamepad handling.
			if (_bridge && active) {
				_bridge->SendToWeb(*active, "ui.gamepad",
					nlohmann::json{ { "kind", "button" }, { "id", e.idCode }, { "down", e.down } });
			}
			if (raw || !e.down) {
				continue;  // raw mode = page owns it; else act on the press edge only
			}
			switch (e.idCode) {
			case XInputButton::kDPadUp:    tap(0x26); break;  // VK_UP
			case XInputButton::kDPadDown:  tap(0x28); break;  // VK_DOWN
			case XInputButton::kDPadLeft:  tap(0x25); break;  // VK_LEFT
			case XInputButton::kDPadRight: tap(0x27); break;  // VK_RIGHT
			case XInputButton::kA:         tap(0x0D); break;  // VK_RETURN — activate
			case XInputButton::kB:         EnqueueMenuRequest(MenuReq::CloseTop); break;  // back — close overlay
			default: break;  // shoulders/thumbs/Start/Back -> raw event only
			}
		}

		if (!captured) {
			// Reset routing timers so the next overlay open starts fresh.
			for (auto& t : _padNavNextRepeat) {
				t = 0.0;
			}
			_padScrollAccum = 0.0f;
			return;
		}

		// ---- sticks (latest deflection) ----
		const auto            s = EngineInput::GetSticks();
		constexpr float       kDeadzone = 0.25f;

		// Raw stick events, throttled to meaningful change (so a page can drive
		// e.g. camera orbit off the raw values).
		if (_bridge && active) {
			const float cur[4] = { s.lx, s.ly, s.rx, s.ry };
			bool        changed = false;
			for (int i = 0; i < 4; ++i) {
				changed = changed || std::fabs(cur[i] - _padLastSentSticks[i]) > 0.04f;
			}
			if (changed) {
				_bridge->SendToWeb(*active, "ui.gamepad",
					nlohmann::json{ { "kind", "stick" }, { "lx", s.lx }, { "ly", s.ly },
						{ "rx", s.rx }, { "ry", s.ry } });
				for (int i = 0; i < 4; ++i) {
					_padLastSentSticks[i] = cur[i];
				}
			}
		}

		if (raw) {
			return;  // no default stick mapping in raw mode
		}

		// Left stick -> arrow-key nav, initial-delay + repeat, per direction
		// (0=up,1=down,2=left,3=right; timer 0.0 == inactive/fresh sentinel).
		constexpr double    kNavInitialDelay = 0.35;
		constexpr double    kNavRepeat = 0.11;
		const bool          dirActive[4] = { s.ly > kDeadzone, s.ly < -kDeadzone,
            s.lx < -kDeadzone, s.lx > kDeadzone };
		const std::uint32_t dirVk[4] = { 0x26, 0x28, 0x25, 0x27 };
		for (int i = 0; i < 4; ++i) {
			if (!dirActive[i]) {
				_padNavNextRepeat[i] = 0.0;
				continue;
			}
			if (_padNavNextRepeat[i] == 0.0) {
				tap(dirVk[i]);
				_padNavNextRepeat[i] = _uptime + kNavInitialDelay;
			} else if (_uptime >= _padNavNextRepeat[i]) {
				tap(dirVk[i]);
				_padNavNextRepeat[i] = _uptime + kNavRepeat;
			}
		}

		// Right stick Y -> scroll. Accumulate fractional notches for smooth,
		// framerate-independent scrolling; +y (stick up) = positive wheel delta
		// = scroll up (sign is easy to flip if it feels inverted in-game).
		if (std::fabs(s.ry) > kDeadzone) {
			constexpr float kScrollNotchesPerSec = 8.0f;
			_padScrollAccum += s.ry * kScrollNotchesPerSec * static_cast<float>(a_deltaSeconds);
			if (const int notches = static_cast<int>(_padScrollAccum); notches != 0) {
				_renderer->InjectMouseWheel(static_cast<int>(_cursorX), static_cast<int>(_cursorY), notches * 120);
				_padScrollAccum -= static_cast<float>(notches);
			}
		} else {
			_padScrollAccum = 0.0f;
		}
	}

	void Runtime::ReconcileControlLayer()
	{
		// Main-thread (Tick). Drive the input-enable layer toward the top menu's CAPTURE policy; this is the ONLY gate that stops gamepad/XInput,
		// so it must track capture (not pause), or a gamepad drives the game underneath a capturing menu.
		// A live HUD (no capture) leaves controls enabled. Engage() may no-op until gameplay (manager not ready at the main menu); IsEngaged() stays false then, so we simply retry next tick.
		// Gated on config.disableControls so turning the setting off releases any
		// engaged lock (this fn is now called every tick, not behind an if).
		const bool wantEngaged = _config.disableControls && _menus.DesiredCapture();
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
		auto settings = std::make_unique<SettingsModule>(schemaDir, valuesDir,
			[this](std::string_view a_mod, std::string_view a_key, const nlohmann::json& a_value) {
				OnSettingChanged(a_mod, a_key, a_value);
			});
		_settings = settings.get();  // core needs schema facts (e.g. key-capture gating)

		// ABI feed (mcm-design.md §8.2): every committed value — including the
		// OnStart NotifyAll replay below and the per-mod replay after an
		// incremental RegisterSchema — lands in the any-thread mirror the C ABI
		// typed getters read, then queues for SubscribeSettings consumers
		// (drained on the main thread by BridgeApi::PumpMainThread). Mirror
		// FIRST: a subscribe replay snapshots the mirror, so it must never lag
		// the queued event. Registry shape changes rebuild the mirror from the
		// store document so a removed mod's values stop resolving.
		auto& store = _settings->Store();
		store.AddChangeListener([](std::string_view a_mod, std::string_view a_key, const nlohmann::json& a_value) {
			auto& api = API::BridgeApi::Get();
			api.Mirror().Update(a_mod, a_key, a_value);
			api.Subscriptions().OnChanged(a_mod, a_key, a_value);
		});
		store.AddRegistryListener([this] {
			if (_settings) {  // teardown guard (_settings nulls before modules die)
				API::BridgeApi::Get().Mirror().Rebuild(_settings->Store().Data());
			}
		});

		// HotkeyService (mcm-design.md §9): every key-typed setting is a live,
		// dispatchable binding. The registry rebuilds on any key-typed commit
		// (a rebind through ANY writer — web, ABI, reset) and on registry
		// shape change; the store's informational conflict grouping shares the
		// same key-name resolution so the store itself stays input-agnostic.
		// Suppression reads the SAME capture state OnHostKey consults, so a
		// press while the user types in a settings field or mid-rebind can
		// never fire a hotkey.
		store.SetKeyNameResolver(ResolveKeyName);

		// Vanilla hotkeys (mcm-design.md §9, v1 — no engine RE): the game's
		// own bindings join the informational conflict grouping as "@game"
		// pseudo-entries. Defaults come from the curated shipped table (the
		// engine bakes its defaults into the executable — no controlmap ships
		// in the archives), then the SAME controlmap text files the engine
		// honors overlay it: a mod-provided Data override, then the user's
		// in-game remaps. Load-time snapshot; live ControlMap reads await RE.
		if (_config.vanillaKeyConflicts) {
			VanillaKeys vanilla;
			if (vanilla.LoadDefaults(Paths::DataDir() / "vanillakeys.json", ResolveKeyName)) {
				const auto scanToVk = [](std::uint32_t a_sc) { return Platform::DirectInputScanToVk(a_sc); };
				// DataDir = <Data>/SFSE/Plugins/OSFUI; under MO2 the module
				// path is virtualized, so this resolves through the VFS too.
				const auto gameData = Paths::DataDir().parent_path().parent_path().parent_path();
				vanilla.OverlayControlMap(gameData / "Interface" / "Controls" / "PC" / "ControlMap.txt", scanToVk);
				if (!docs.empty()) {
					vanilla.OverlayControlMap(docs / "My Games" / "Starfield" / "ControlMap_Custom.txt", scanToVk);
				}
				std::vector<SettingsStore::VanillaKey> keys;
				for (const auto& b : vanilla.Bindings()) {
					if (b.vk != 0) {
						// name AFTER the overlays: a rebound event displays
						// its live key, not the curated default's spelling.
						keys.push_back({ b.event, "Starfield (" + b.label + ")", b.vk, KeyName(b.vk) });
					}
				}
				store.SetVanillaKeys(std::move(keys));
			}
		}

		_hotkeys.SetSuppression([this] { return IsInputCaptured() || _captureArmed.load(); });
		store.AddChangeListener([this](std::string_view a_mod, std::string_view a_key, const nlohmann::json&) {
			if (_settings && _settings->Store().GetSettingType(a_mod, a_key) == "key") {
				_hotkeys.Rebuild(_settings->Store());
			}
		});
		store.AddRegistryListener([this] {
			if (_settings) {
				_hotkeys.Rebuild(_settings->Store());
			}
		});
		_hotkeys.Rebuild(store);  // LoadAll already ran in the module's constructor

		_modules.push_back(std::move(settings));

		REX::INFO("Runtime: {} UI module(s) loaded", _modules.size());
	}

	void Runtime::DrainKeyCapture()
	{
		const KeyCode vk = _capturedVk.exchange(kInvalidKeyCode);
		if (vk == kInvalidKeyCode) {
			return;  // nothing captured this tick
		}
		if (!_bridge || _captureView.empty()) {
			return;  // nobody to answer
		}
		// Escape cancels the rebind; an unnameable VK can't be a binding.
		constexpr KeyCode kVkEscape = 0x1B;
		const std::string name = (vk == kVkEscape) ? std::string{} : KeyName(vk);
		const bool cancelled = name.empty();
		// Tell the view which setting + the captured name; it echoes back a normal
		// settings.set (so the store persists + OnSettingChanged re-resolves).
		nlohmann::json payload{
			{ "mod", _captureMod },
			{ "key", _captureKey },
			{ "name", name },
			{ "cancelled", cancelled },
		};
		// Live-warn during capture (mcm-design.md §9): which OTHER key-typed
		// settings already sit on the captured key, so the rebind UI warns
		// BEFORE the view commits. The store still holds the OLD binding for
		// this setting (the commit is the view's echo), so exclude self.
		// Informational, never blocking — same contract as Data()'s badges.
		if (!cancelled && _settings) {
			if (auto conflicts = _settings->Store().ConflictsFor(vk, _captureMod, _captureKey); !conflicts.empty()) {
				payload["conflicts"] = std::move(conflicts);
			}
		}
		_bridge->SendToWeb(_captureView, "settings.captured", payload);
		REX::INFO("Runtime: key capture -> {} ({}.{})", cancelled ? "(cancelled)" : name, _captureMod, _captureKey);
		_captureView.clear();
		_captureMod.clear();
		_captureKey.clear();
	}

	void Runtime::DrainHotkeys()
	{
		_hotkeys.Drain([this](const std::string& a_mod, const std::string& a_key) {
			// Both delivery channels (mcm-design.md §9): C ABI subscribers
			// (queued here, invoked unlocked by BridgeApi::PumpMainThread
			// later this tick) and the web `ui.hotkey` push to settings
			// subscribers.
			API::BridgeApi::Get().Hotkeys().OnFired(a_mod, a_key);
			if (_settings) {
				_settings->PushHotkey(a_mod, a_key);
			}
			if (Log::DevMode()) {
				REX::DEBUG("Runtime: hotkey fired for {}.{}", a_mod, a_key);
			}
		});
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
		// Catalog of loaded surfaces (the hub view's read, bridge 0.2). Replies
		// with `views.data` and SUBSCRIBES the caller: any later open/close/
		// focus/load-state change re-sends the catalog (see BroadcastViewsData),
		// so a hub reflects state without polling.
		a_bridge.RegisterCommand("views.get", [this](const nlohmann::json&, MessageBridge& a_b) {
			const auto payload = BuildViewsData();
			_viewsSubscribers.insert(std::string(a_b.CurrentSource()));
			_lastViewsData = payload.dump();
			a_b.SendToWeb("views.data", payload);
		});
		// Arm key-rebind capture: the NEXT key press is grabbed by OnHostKey and
		// reported back via `settings.captured`. Any setting a loaded schema
		// declares `type:"key"` is rebindable — the schema fact gates the
		// capture, not an allowlist. Runs on the main thread; OnHostKey (window
		// thread) reads the armed flag.
		a_bridge.RegisterCommand("settings.captureKey", [this](const nlohmann::json& a_p, MessageBridge& a_b) {
			const std::string mod = Json::GetString(a_p, "mod", "");
			const std::string key = Json::GetString(a_p, "key", "");
			if (!_settings || _settings->Store().GetSettingType(mod, key) != "key") {
				REX::WARN("Runtime: settings.captureKey rejected — '{}.{}' is not a key-typed setting",
					mod.substr(0, 64), key.substr(0, 64));
				// Reply cancelled so the view's rebind button restores instead of
				// dead-waiting into its timeout toast.
				a_b.SendToWeb("settings.captured", nlohmann::json{
					{ "mod", mod }, { "key", key }, { "name", "" }, { "cancelled", true } });
				return;
			}
			_captureView = std::string(a_b.CurrentSource());
			_captureMod = mod;
			_captureKey = key;
			_captureArmed.store(true);
			REX::INFO("Runtime: armed key capture for {}.{} (from view '{}')", mod, key, _captureView);
		});
		a_bridge.RegisterCommand("log", [](const nlohmann::json& a_p, MessageBridge&) {
			// Untrusted content: bound the length so JS cannot flood the log.
			REX::INFO("MessageBridge: [web] {}", Json::GetString(a_p, "text", "").substr(0, 512));
		});
		a_bridge.RegisterCommand("ping", [](const nlohmann::json&, MessageBridge& a_b) {
			a_b.SendToWeb("runtime.pong", nlohmann::json::object());
		});
		a_bridge.RegisterCommand("osfui.gamepadRaw", [](const nlohmann::json& a_p, MessageBridge&) {
			// A page that wants to own the gamepad (e.g. stick-driven camera
			// orbit) sets this to suppress the default nav/scroll mapping; it
			// then handles the raw `ui.gamepad` events itself. PER-SESSION: the
			// runtime resets it on overlay close — re-assert on each
			// ui.visibility show.
			EngineInput::SetRawMode(Json::GetBool(a_p, "raw", false));
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
		// The Phase 5b payoff: settings drive native behaviour live. Only the
		// framework's own knobs (mod "osfui") are handled here; other mods'
		// settings are theirs to react to. Fires on the MAIN thread (settings
		// commands dispatch from Tick), and once per value at startup via the
		// settings module's NotifyAll — so a persisted choice applies on boot.
		if (a_modId != "osfui") {
			return;
		}
		// Toggle key rebind: re-resolve the name and re-apply it to the input
		// router. Reject an unresolvable name (keep the working key) rather than
		// disabling the toggle. Fires on the main thread (settings dispatch), so
		// re-Configuring the router is as safe as the initial Configure.
		if (a_key == "toggleKey" && a_value.is_string()) {
			const auto name = a_value.get<std::string>();
			const auto vk = ResolveKeyName(name);
			if (vk == kInvalidKeyCode) {
				REX::WARN("Runtime: setting osfui.toggleKey '{}' is not a resolvable key; keeping '{}'", name, _config.toggleKey);
				return;
			}
			_config.toggleKey = name;
			_toggleKey = vk;
			ApplyToggleKey();
			REX::INFO("Runtime: setting osfui.toggleKey -> {} (VK {:#x})", name, vk);
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
			if (_revealPending) {
				if (frame->frameIndex == _lastSubmittedFrame) {
					// Still the frame from before the open — the renderer
					// republishes under a new serial once the (re)shown view is
					// presentable (messages delivered), so hold the reveal.
					return;
				}
				_lastSubmittedFrame = frame->frameIndex;
				_compositor->Submit(*frame);  // cache the fresh texture first...
				_revealPending = false;
				_compositor->SetVisible(true);  // ...then let the present hook draw it
				return;
			}
			_lastSubmittedFrame = frame->frameIndex;
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
