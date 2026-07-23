#include "runtime/Runtime.h"

#include <cmath>

#include "RE/C/Calendar.h"

#include "api/BridgeApi.h"
#include "api/PapyrusApi.h"
#include "composite/D3D12Compositor.h"
#include "composite/NullCompositor.h"
#include "composite/UiPassSeam.h"
#include "core/Log.h"
#include "core/Version.h"
#include "input/ControlLayer.h"
#include "input/EngineInput.h"
#include "input/FocusMenu.h"
#include "input/FreeCursor.h"
#include "input/HardwareCursor.h"
#include "input/MainThreadMenuPump.h"
#include "input/MenuMode.h"
#include "input/OverlayInputHook.h"
#include "input/PauseMenuEntry.h"
#include "input/SimPause.h"
#include "input/XInputPoller.h"
#include "core/Paths.h"
#include "platform/WindowsPlatform.h"
#include "render/MockWebRenderer.h"
#include "runtime/Json.h"
#include "runtime/Ids.h"
#include "runtime/VanillaKeys.h"
#include "render/NullWebRenderer.h"
#include "render/WebView2HostWebRenderer.h"

namespace OSFUI
{
	namespace
	{
		constexpr std::string_view kHandoffViewId{ "osfui/handoff" };
		constexpr double           kHandoffDelaySeconds{ 0.15 };
		constexpr double           kReadySignalTimeoutSeconds{ 15.0 };
	}

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
		const auto documents = Platform::GetDocumentsPath();
		const auto starfieldDir = documents.empty() ? std::filesystem::path{} :
			documents / "My Games" / "Starfield";
		_localization.Load(Paths::DataDir() / "l10n",
			LocalizationService::DetectGameLocale(starfieldDir));

		// Label + target view for the injected PauseMenu entry; the main-thread
		// pump gates Reconcile on config.pauseMenuEntry via SetEnabled.
		PauseMenuEntry::Configure(
			_localization.Resolve("osfui", "chrome.pauseMenuEntry", _config.pauseMenuEntryLabel),
			_config.pauseMenuEntryView);
		PauseMenuEntry::SetEnabled(_config.pauseMenuEntry);

		_views.LoadAll(Paths::ViewsDir());
		std::vector<std::string> discoveredViewIds;
		discoveredViewIds.reserve(_views.All().size());
		for (const auto& manifest : _views.All()) {
			discoveredViewIds.push_back(manifest.id);
		}
		API::BridgeApi::Get().SetViewCatalog(discoveredViewIds);

		_renderer = CreateRenderer();
		const auto* view = _views.Find(_config.view);
		const auto initialWidth = view ? view->width : kDefaultViewWidth;
		const auto initialHeight = view ? view->height : kDefaultViewHeight;
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

		// A failed load never fires DOM-ready, so this is the only signal a view
		// didn't come up. Drives crash-recovery.
		_renderer->SetLoadHandler([this](const IWebRenderer::LoadEvent& a_e) {
			OnViewLoad(a_e.viewId, a_e.failed, a_e.url, a_e.description, a_e.errorCode);
		});

		// The active page's CSS `cursor` drives the real OS pointer. Unlike the
		// other handlers this fires on the renderer's worker thread (IWebRenderer
		// contract) — SetShape is one atomic store, applied by the WndProc hook on
		// the next mouse message. hardwareCursor is a boot-time config knob, not a
		// setting; the only alternative is an invisible software cursor.
		if (_config.hardwareCursor) {
			_renderer->SetCursorChangeHandler([](CursorShape a_shape) {
				HardwareCursor::SetShape(a_shape);
			});
		}

		_compositor = CreateCompositor();
		if (!_compositor->Initialize()) {
			REX::WARN("Runtime: compositor '{}' failed to initialize; falling back to null compositor", _compositor->Name());
			_compositor = std::make_unique<NullCompositor>();
			_compositor->Initialize();
		}
		// GPU frame transport (out-of-process WebView2 host): the compositor owns
		// the shared-texture ring handles once handed over. Fires on the game
		// thread (renderer Update()); no-op for CPU-only renderer/compositor pairs.
		_renderer->SetSharedRingHandler([this](const SharedRingDesc& a_desc) {
			if (_compositor) {
				_compositor->SetSharedRing(a_desc);
			}
		});
		// Size the view to the real output so the page renders aspect-correct.
		_compositor->SetOutputResizeCallback([this](std::uint32_t a_w, std::uint32_t a_h) { OnOutputResized(a_w, a_h); });

		if (_config.uiPassDraw) {
			// The release path records into Starfield's transparent Scaleform UI
			// layer, upstream of both real-frame composition and Frame Generation.
			// Vtables are static .rdata, so installation does not wait for the
			// renderer root like the D3D12 compositor does.
			const bool seamReady = UiPassSeam::Install(_config.uiPassDraw);
			if (seamReady) {
				// The present hook stops drawing and becomes plumbing only.
				_compositor->SetSeamDrawMode(true);
			} else {
				REX::WARN("Runtime: Scaleform seam unavailable — using the legacy present-time overlay; "
						  "Frame Generation will suspend that fallback for safety");
			}
		}
		REX::INFO("Runtime: compositor = {}", _compositor->Name());

		_captureInput.store(_config.captureInput);

		// Composition root for feature modules (hosted generically via IUiModule).
		// OnStart() applies persisted state before the first frame.
		BuildModules();
		for (const auto& module : _modules) {
			module->OnStart();
		}

		// One bridge serves every bridge-enabled view. Build it before any view is
		// loaded so a first-open lazy surface gets exactly the same handler wiring
		// as a boot surface. BridgeApi is told it is ready only when LoadSurface
		// actually creates a nativeBridge surface, preserving its readiness contract
		// and pre-ready SendToWeb queue.
		_bridge = std::make_unique<MessageBridge>([this](std::string_view a_viewId, std::string_view a_json) {
			if (_renderer) {
				_renderer->SendMessageToWeb(a_viewId, a_json);
			}
		});
		RegisterPlatformCommands(*_bridge);
		for (const auto& module : _modules) {
			module->RegisterCommands(*_bridge);
		}
		_renderer->SetWebMessageHandler([this](std::string_view a_viewId, std::string_view a_json) {
			if (_bridge) {
				_bridge->HandleWebMessage(a_viewId, a_json);
			}
		});

		// The first-load handoff is useful only on a renderer that can keep it
		// warm beside a target view. It is a hidden platform surface, loaded
		// independently of config.views so drop-in menus inherit the behavior.
		if (_renderer->SupportsMultipleViews()) {
			if (const auto* handoff = _views.Find(kHandoffViewId)) {
				if (LoadSurface(*handoff, "as the warm first-load handoff")) {
					// Hidden WebView2 controllers normally suspend before their first
					// paint. Prime this one at boot so opening a cold target never also
					// pays the handoff surface's renderer startup cost.
					_renderer->PrewarmView(kHandoffViewId);
				}
			}
		}

		// The bridge and web->native handler must be wired before LoadView so no
		// early page message races past them; the renderer queues native->web
		// messages per view until each page is ready.
		if (view) {
			std::vector<std::string> toLoad = _config.views;
			if (toLoad.empty()) {
				toLoad.push_back(_config.view);
			}
			if (!_renderer->SupportsMultipleViews() && toLoad.size() > 1) {
				REX::WARN("Runtime: renderer '{}' supports one view in this phase; loading only default '{}'",
					_renderer->Name(), _config.view);
				toLoad.assign(1, _config.view);
			}

			// Ordering, focus and visibility are owned by MenuController +
			// ApplyMenuPolicy, not by manifest order or a single active view.
			std::size_t loaded = 0;
			for (const auto& id : toLoad) {
				if (id == kHandoffViewId) {
					continue;  // platform-owned and already loaded above
				}
				if (const auto* m = _views.Find(id)) {
					if (LoadSurface(*m, "config")) {
						++loaded;
					}
					if (m->openOnStart && _menus.IsRegistered(id)) {
						_menus.Open(id);
					}
				} else {
					REX::WARN("Runtime: configured view '{}' not found; skipping", id);
				}
			}
			REX::INFO("Runtime: loaded {} view(s); default menu = '{}'", loaded, _config.view);
			if (!_menus.IsRegistered(_config.view)) {
				REX::WARN("Runtime: default view '{}' is not among the loaded surfaces; the toggle key will have nothing to open (check config.view is listed in config.views)", _config.view);
			}
		} else {
			REX::WARN("Runtime: configured view '{}' was not found; overlay has no content", _config.view);
		}

		// Key events reach the router from the WndProc subclass (OverlayInputHook
		// → OnHostKey), installed when config inputSource="ui" (core/Plugin.cpp,
		// kPostPostDataLoad).
		_toggleKey = ResolveKeyName(_config.toggleKey);
		if (_toggleKey != kInvalidKeyCode) {
			REX::INFO("Runtime: toggleKey '{}' resolved to VK code {:#x}", _config.toggleKey, _toggleKey);
		}
		// Dev view-reload key (mcm-design.md §12.1): resolved only in devMode —
		// kInvalid is the whole gate in OnHostKey, so a user config with the
		// shipped devReloadKey but devMode off never loses the key to us.
		if (_config.devMode && !_config.devReloadKey.empty()) {
			_devReloadKey = ResolveKeyName(_config.devReloadKey);
			if (_devReloadKey != kInvalidKeyCode) {
				REX::DEBUG("Runtime: devReloadKey '{}' resolved to VK code {:#x} (reloads the top open menu)", _config.devReloadKey, _devReloadKey);
			}
		}

		EngineInput::SetEnabled(_config.engineInput);
		if (_config.engineInput) {
			REX::DEBUG("Runtime: engineInput enabled — engine per-menu input (gamepad) routed into the focused view; keyboard/mouse stay on the WndProc path");
		}

		// Toggle key opens/closes the default menu; Esc (while captured) is the back
		// action — close the top menu, or delegate to a back-owning view
		// (osfui.handleBack). Separate so a live rebind can re-apply it.
		ApplyToggleKey();
		_renderer->SetNativeAcceleratorHandler(
			[this](std::uint32_t a_vkCode, bool a_down) {
				return OnNativeAcceleratorKey(a_vkCode, a_down);
			});

		_input.SetWebRouting(
			[this] { return IsInputCaptured(); },
			[this](KeyCode a_key, bool a_down) {
				if (_renderer) {
					_renderer->InjectKeyEvent(a_key, a_down);
				}
			});
		REX::INFO("Runtime: input capture {} (config captureInput)", _config.captureInput ? "enabled" : "disabled");

		_initialized = true;
		// Push the initial policy derived from whatever is open (incl. nothing).
		ApplyMenuPolicy();
		REX::INFO("Runtime: initialized (visible={})", _visible.load());

		return true;
	}

	void Runtime::Shutdown()
	{
		// SFSE provides no plugin shutdown callback; this is only reached if
		// process-detach or an explicit teardown is ever wired. Everything here
		// must stay safe to skip entirely.
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
		// Detach the native plugin API before destroying the bridge, so its
		// non-owning pointer never dangles and it reports not-ready.
		API::BridgeApi::Get().OnBridgeReady(nullptr);
		API::BridgeApi::Get().SetViewCatalog({});
		// Same for modules that retain the bridge for unsolicited pushes.
		for (const auto& module : _modules) {
			module->OnBridgeDown();
		}
		// Bridge before modules: its command handlers capture module pointers,
		// so it must not outlive them.
		_bridge.reset();
		_viewsSubscribers.clear();
		_i18nSubscribers.clear();
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
		// The pause-menu entry (PauseMenuEntry::Reconcile) is NOT driven from
		// here anymore: SFSE tasks run on a render-graph worker, not the game
		// main thread, and its Scaleform access raced the AS3 VM (CTD family,
		// trainwreck-proven 2026-07-23). MainThreadMenuPump drives it post
		// UI_AdvanceActiveMenus on the owning thread; a click's EnqueueOpenView
		// is thread-safe and lands in the snapshot below on the next tick.
		// Register plugin-supplied views (ABI 1.5) before the menu-request snapshot
		// below, so a RegisterView followed by RequestMenu in the same frame finds
		// its surface registered when the request is applied.
		DrainViewRegistrations();
		// Snapshot queued menu requests (toggle/Esc/transition + plugin
		// RequestMenu) now, but apply them after the bridge pump below — the ABI
		// 1.3 ordering guarantee: a consumer that called SendToWeb(v, ...) then
		// RequestMenu(v, true) has its send in _pendingSends before the request
		// entered this snapshot, so the pump flushes the message into v's queue
		// before the open unhides v (message before first visible paint).
		const auto menuWork = TakeMenuRequests();
		// Load discovered targets while they are still hidden, before queued sends
		// are pumped. ApplyMenuRequests performs only the visibility transition.
		PrepareMenuRequests(menuWork);
		// Deliver a captured rebind key back to the settings view (main thread).
		DrainKeyCapture();
		// Deliver queued hotkey fires (window thread -> main, mcm-design.md §9)
		// before the bridge pump below, so the C ABI callbacks they queue are
		// invoked this same tick.
		DrainHotkeys();
		// Apply queued runtime schema (un)registrations to the store first, so
		// their value replay is already queued when the pump below drains
		// SubscribeSettings callbacks — registration lands in one tick.
		DrainSchemaOps();
		// Papyrus Set*/Reset ops (mcm-design.md §8.4) go through the same validated
		// store path as every other writer. After DrainSchemaOps so a set against a
		// just-registered schema resolves this tick.
		if (_settings) {
			API::Papyrus::DrainSettingsOps(_settings->Store());
		}
		// Papyrus PushToView payloads fan out to the pushing mod's live views as
		// `data.push` — before PumpMainThread/Update flush the per-view outbound
		// queues, so a push lands in this tick's frame. No subscriber set: the
		// target list is derived fresh from the live surfaces each time, so there
		// is nothing to prune or go stale.
		if (_bridge) {
			API::Papyrus::DrainViewPushes([this](const API::Papyrus::ViewPush& a_push) {
				std::unordered_set<std::string> targets;
				const std::string               prefix = a_push.mod + "/";
				for (const auto& m : _views.All()) {
					if (_menus.IsRegistered(m.id) && m.id.starts_with(prefix)) {
						targets.insert(m.id);
					}
				}
				if (targets.empty()) {
					// A mod pushing with no view installed/live is not an error;
					// leave a devMode trace.
					REX::DEBUG("Runtime: PushToView {}.{} had no live '{}/...' view to deliver to",
						a_push.mod, a_push.key, a_push.mod);
					return;
				}
				nlohmann::json payload{
					{ "mod", a_push.mod }, { "key", a_push.key }, { "values", a_push.values }
				};
				if (a_push.forms) {
					// PushFormsToView (protocol 1.3): serialized form identity
					// objects ride the same data.push as an additive field.
					payload["forms"] = *a_push.forms;
				}
				_bridge->SendToWeb(targets, "data.push", std::move(payload));
			});
		}
		// Apply the native plugin API's queued ops (command (re)registration +
		// off-thread SendToWeb) on the main thread, before Update() flushes the
		// per-view outbound queues to the pages.
		API::BridgeApi::Get().PumpMainThread();
		// Apply the snapshot now, so the reconcilers below and the frame submitted
		// this tick reflect the new menu state.
		ApplyMenuRequests(menuWork);
		// Land coalesced settings value writes once their write-behind window
		// elapses (mcm-design.md §8.1) — a slider drag costs one disk write per
		// ~500ms, not one per step.
		if (_settings) {
			_settings->Store().PumpPersistence(_uptime);
			// Schema hot-reload (mcm-design.md §12.1, devMode): edited
			// settings/*.json files reload live, values preserved; the
			// registry re-broadcast repaints any open settings view.
			if (_config.devMode) {
				_settings->PumpSchemaHotReload(_uptime);
				if (_uptime >= _nextLocalizationScan) {
					_nextLocalizationScan = _uptime + SettingsModule::kHotReloadScanSeconds;
					if (_localization.ReloadIfChanged()) {
						RefreshLocalizedData();
					}
				}
			}
		}
		// Reconcile engine menu-mode + control-disable toward the derived capture
		// state (not visibility): a live HUD must not disable controls.
		if (_config.focusMenu) {
			ReconcileFocusMenu();
		}
		// Unconditional, so losing capture releases any engaged lock (a gate here
		// would stop reconciling and strand the player's controls).
		ReconcileControlLayer();
		// Sim pause (manifest pausesGame) — unconditional: a direct
		// Main::isGameMenuPaused write, independent of the engine focus menu.
		ReconcileSimPause();
		// OS-cursor release — unconditional, tracks capture (the same policy that
		// activates the hardware cursor): while a menu captures input, hold a
		// reference on MenuCursor::freeCursorRefCount so the per-frame clip
		// releases the pointer (no engine arrow — the focus menu carries no
		// ShowCursor bit). Edge-triggered inside Apply, which marshals the ref bump
		// onto the main thread (Tick runs off-main; proven 2026-07-23).
		FreeCursor::Apply(_menus.DesiredCapture());
		if (_config.engineInput) {
			DrainEngineInput(a_deltaSeconds);
		}
		if (!_renderer) {
			return;
		}
		// Fire any due crash-recovery reloads before Update pumps the renderer.
		DriveRecovery();
		DriveDevReload();
		// Flush the coalesced mouse move (QueueMouseMove): one injected move
		// per frame carrying the latest position, however many raw packets the
		// window thread recorded since the last tick.
		if (const auto packed = _pendingMouseMove.exchange(kNoPendingMouseMove);
			packed != kNoPendingMouseMove) {
			_renderer->InjectMouseMove(
				static_cast<int>(packed >> 32),
				static_cast<int>(packed & 0xFFFF'FFFFull));
			++_mouseMoveSends;
		}
		if (_config.devMode && _uptime >= _nextMouseStatsLog) {
			_nextMouseStatsLog = _uptime + 5.0;
			const auto packets = _mouseMovePackets.exchange(0, std::memory_order_relaxed);
			if (packets != 0 || _mouseMoveSends != 0) {
				REX::DEBUG("Runtime: coalesced {} mouse-move packets into {} sends over ~5s",
					packets, _mouseMoveSends);
				_mouseMoveSends = 0;
			}
		}
		{
			// Out-of-process backends mirror the accelerator state so their host
			// process can decide `handled` synchronously; pushed every tick,
			// backends diff and forward only changes (default no-op).
			_renderer->SetAcceleratorKeys(_toggleKey, _devReloadKey,
				IsInputCaptured(), _captureArmed.load(), _captureUpVk.load());
			_renderer->Update(a_deltaSeconds);
			DrivePendingOpen();
			SubmitFrameIfVisible();
			UpdateRenderDiagnostics();
		}
	}

	void Runtime::EnqueueMenuRequest(MenuReq a_req)
	{
		// Callable from any thread (WndProc toggle/Esc, MenuEventSink transition).
		// Leaf lock: it only guards the queue; the request is acted on in Tick.
		std::lock_guard lock(_reqMutex);
		_reqs.push_back(a_req);
	}

	void Runtime::EnqueueOpenView(std::string a_viewId)
	{
		// Callable from any thread (PauseMenuEntry click). Same leaf-lock
		// discipline as EnqueueMenuRequest.
		std::lock_guard lock(_reqMutex);
		_openViewReqs.push_back(std::move(a_viewId));
	}

	bool Runtime::LoadSurface(const ViewManifest& a_manifest, std::string_view a_reason)
	{
		const auto& id = a_manifest.id;
		if (_menus.IsRegistered(id)) {
			return true;
		}
		if (!_renderer) {
			return false;
		}
		if (!_renderer->SupportsMultipleViews() && !_menus.DesiredLayers().empty()) {
			REX::WARN("Runtime: cannot load surface '{}' on demand — renderer '{}' is single-view",
				id, _renderer->Name());
			return false;
		}

		// Install diagnostics before navigation so even the earliest page console
		// output is captured. The handler survives recovery reloads until the view
		// is explicitly destroyed.
		if (_config.devMode) {
			_renderer->SetConsoleHandler(id, [id](int a_level, std::string a_message) {
				if (a_level == 2) {
					REX::ERROR("Runtime: view '{}' console: {}", id, a_message);
				} else if (a_level == 1) {
					REX::WARN("Runtime: view '{}' console: {}", id, a_message);
				} else {
					REX::INFO("Runtime: view '{}' console: {}", id, a_message);
				}
			});
		}

		_recovery.erase(id);
		_viewLoadState[id] = ViewLoadState::Loading;
		_readyViews.erase(id);
		_renderer->LoadView(a_manifest);
		_renderer->SetRenderStats(id, _renderStatsEnabled);
		// A fresh view starts at manifest dimensions; restore the current
		// output-matched size. Before first present these are the initialized
		// logical dimensions and the normal output-resize path supersedes them.
		if (const auto w = _viewWidth.load(), h = _viewHeight.load(); w && h) {
			_renderer->Resize(w, h);
		}
		_menus.Register({ id, a_manifest.kind, a_manifest.capturesInput,
			a_manifest.pausesGame, a_manifest.order });
		API::BridgeApi::Get().SetSurfaceLoaded(id, true);

		REX::INFO("Runtime: surface '{}' loaded {} ({}, capturesInput={}, pausesGame={})",
			id, a_reason, a_manifest.kind == SurfaceKind::Hud ? "hud" : "menu",
			a_manifest.capturesInput, a_manifest.pausesGame);
		if (a_manifest.permissions.nativeBridge && _bridge) {
			// This may be the first bridge-enabled surface. Publish the bridge before
			// this tick's PumpMainThread so queued SendToWeb work reaches the newly
			// created renderer view.
			API::BridgeApi::Get().OnBridgeReady(_bridge.get());
			_bridge->SendRuntimeReady(id);
		}
		return true;
	}

	Runtime::PendingMenuWork Runtime::TakeMenuRequests()
	{
		// Snapshot under the lock, then act unlocked (in ApplyMenuRequests): the
		// actions call into the renderer/compositor and must never run while
		// holding _reqMutex.
		PendingMenuWork work;
		{
			std::lock_guard lock(_reqMutex);
			work.local.swap(_reqs);
			work.openViews.swap(_openViewReqs);
		}
		// Sibling-plugin opens/closes by id; same policy path as the toggle key.
		work.plugin = API::BridgeApi::Get().TakeMenuRequests();
		return work;
	}

	void Runtime::PrepareMenuRequests(const PendingMenuWork& a_work)
	{
		const auto prepare = [this](std::string_view a_id, std::string_view a_reason) {
			if (_menus.IsRegistered(a_id)) {
				return;
			}
			if (const auto* manifest = _views.Find(a_id)) {
				LoadSurface(*manifest, a_reason);
			}
		};

		for (const auto& id : a_work.openViews) {
			prepare(id, "on demand");
		}
		for (const auto& request : a_work.plugin) {
			if (request.open) {
				prepare(request.view, "on demand");
			}
		}
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
				if (_pendingSurfaceOpen) {
					CancelPendingOpen();
				} else if (_menus.ActiveMenu()) {
					_menus.CloseTop();
				} else {
					BeginSurfaceOpen(_config.view);
				}
				break;
			case MenuReq::Back: {
				// Esc / pad-B. A back-owning active view (osfui.handleBack) gets
				// the action delegated as a synthetic Escape tap and decides for
				// itself — navigate elsewhere, peel an inner panel, or send
				// `close`. Everyone else closes the top menu (single-menu policy:
				// that hides the overlay). The toggle key never delegates, so a
				// broken page cannot strand the user.
				const auto active = _menus.ActiveMenu();
				if (_pendingSurfaceOpen && (!active || *active == kHandoffViewId)) {
					CancelPendingOpen();
				} else if (active && _backOwnerViews.contains(*active) && _renderer) {
					constexpr std::uint32_t kVkEscape = 0x1B;
					_renderer->InjectKeyEvent(kVkEscape, true);
					_renderer->InjectKeyEvent(kVkEscape, false);
				} else {
					_menus.CloseTop();
				}
				break;
			}
			case MenuReq::CloseTop:
				if (_pendingSurfaceOpen) {
					CancelPendingOpen();
				} else {
					_menus.CloseTop();
				}
				break;
			case MenuReq::CloseAll:
				CancelPendingOpen();
				_menus.CloseAll();
				break;
			}
		}
		for (const auto& id : a_work.openViews) {
			if (!_menus.IsRegistered(id)) {
				REX::WARN("Runtime: EnqueueOpenView('{}') ignored — no discovered surface could be loaded", id);
			} else {
				BeginSurfaceOpen(id);
			}
		}
		for (const auto& r : pluginReqs) {
			if (r.open) {
				if (!_menus.IsRegistered(r.view)) {
					REX::WARN("Runtime: plugin RequestMenu('{}', open) could not load the discovered surface", r.view);
				} else {
					BeginSurfaceOpen(r.view);
				}
			} else {
				if (_pendingSurfaceOpen &&
					(_pendingSurfaceOpen->target == r.view || r.view == kHandoffViewId)) {
					CancelPendingOpen();
				}
				_menus.Close(r.view);
			}
		}
		ApplyMenuPolicy();
	}

	bool Runtime::BeginSurfaceOpen(std::string_view a_id)
	{
		if (!_menus.IsRegistered(a_id)) {
			return false;
		}
		const auto* manifest = _views.Find(a_id);
		if (!manifest || manifest->kind == SurfaceKind::Hud ||
			a_id == kHandoffViewId || !_menus.IsRegistered(kHandoffViewId)) {
			CancelPendingOpen();
			return _menus.Open(a_id);
		}

		const auto loadState = GetViewLoadState(a_id);
		const bool ready = manifest->readySignal ?
			_readyViews.contains(std::string(a_id)) :
			loadState == ViewLoadState::Finished;
		if (ready) {
			CancelPendingOpen();
			return _menus.Open(a_id);
		}
		if (_pendingSurfaceOpen && _pendingSurfaceOpen->target == a_id) {
			return false;
		}

		CancelPendingOpen();
		PendingSurfaceOpen pending;
		pending.target = std::string(a_id);
		pending.startedAt = _uptime;
		if (loadState == ViewLoadState::Finished) {
			pending.loadedAt = _uptime;
		}
		_pendingSurfaceOpen = std::move(pending);
		REX::DEBUG("Runtime: holding first open of '{}' until the view is ready", a_id);
		return true;
	}

	bool Runtime::CancelPendingOpen()
	{
		if (!_pendingSurfaceOpen) {
			return false;
		}
		const auto target = _pendingSurfaceOpen->target;
		const bool changed = _menus.Close(kHandoffViewId);
		_pendingSurfaceOpen.reset();
		REX::DEBUG("Runtime: cancelled pending open of '{}'", target);
		return changed;
	}

	void Runtime::ShowHandoff(std::string_view a_phase, bool a_retry)
	{
		if (!_pendingSurfaceOpen || !_bridge) {
			return;
		}
		auto& pending = *_pendingSurfaceOpen;
		const auto* target = _views.Find(pending.target);
		if (!target || !_menus.IsRegistered(kHandoffViewId)) {
			return;
		}
		const bool stateChanged = !pending.handoffVisible || pending.phase != a_phase ||
			pending.error != a_retry;
		if (!stateChanged) {
			return;
		}

		// The warm surface borrows the target menu's policy, so loading feels
		// like entering that same terminal instead of opening global UI chrome.
		_menus.Register({ std::string(kHandoffViewId), SurfaceKind::Menu,
			target->capturesInput, target->pausesGame, target->order });
		const auto slash = target->id.find('/');
		const auto viewName = slash == std::string::npos ? target->id : target->id.substr(slash + 1);
		const auto title = _localization.Resolve(target->mod,
			"views." + viewName + ".title", target->title);
		_bridge->SendToWeb(kHandoffViewId, "handoff.state", nlohmann::json{
			{ "target", target->id },
			{ "mod", target->mod },
			{ "title", title },
			{ "accent", target->accent },
			{ "phase", a_phase },
			{ "retry", a_retry },
		});
		_menus.Open(kHandoffViewId);
		pending.handoffVisible = true;
		pending.phase = std::string(a_phase);
		pending.error = a_retry;
		ApplyMenuPolicy();
	}

	void Runtime::FinishPendingOpen()
	{
		if (!_pendingSurfaceOpen) {
			return;
		}
		const auto target = _pendingSurfaceOpen->target;
		_menus.Close(kHandoffViewId);
		_menus.Open(target);
		_pendingSurfaceOpen.reset();
		REX::DEBUG("Runtime: first-load handoff completed for '{}'", target);
		ApplyMenuPolicy();
	}

	void Runtime::DrivePendingOpen()
	{
		if (!_pendingSurfaceOpen) {
			return;
		}
		auto& pending = *_pendingSurfaceOpen;
		const auto* manifest = _views.Find(pending.target);
		if (!manifest || !_menus.IsRegistered(pending.target)) {
			ShowHandoff("error", true);
			return;
		}
		if (pending.retryRequested) {
			pending.retryRequested = false;
			if (!_renderer) {
				return;
			}
			if (!_menus.IsRegistered(pending.target)) {
				if (!LoadSurface(*manifest, "for first-load handoff retry")) {
					ShowHandoff("error", true);
					return;
				}
			} else {
				_recovery.erase(pending.target);
				_viewLoadState[pending.target] = ViewLoadState::Loading;
				_readyViews.erase(pending.target);
				_renderer->LoadView(*manifest);
				_renderer->Resize(_viewWidth.load(), _viewHeight.load());
				if (manifest->permissions.nativeBridge && _bridge) {
					_bridge->SendRuntimeReady(pending.target);
				}
			}
			pending.startedAt = _uptime;
			pending.loadedAt = -1.0;
			pending.phase.clear();
			pending.error = false;
			ShowHandoff("linking", false);
			BroadcastViewsData();
			return;
		}

		const auto state = GetViewLoadState(pending.target);
		if (state == ViewLoadState::Finished && pending.loadedAt < 0.0) {
			pending.loadedAt = _uptime;
		}
		const bool ready = manifest->readySignal ?
			_readyViews.contains(pending.target) :
			state == ViewLoadState::Finished;
		if (ready) {
			FinishPendingOpen();
			return;
		}
		if (manifest->readySignal && pending.loadedAt >= 0.0 &&
			_uptime - pending.loadedAt >= kReadySignalTimeoutSeconds) {
			ShowHandoff("error", true);
			return;
		}
		if (_uptime - pending.startedAt < kHandoffDelaySeconds) {
			return;
		}
		ShowHandoff(state == ViewLoadState::Failed ? "retrying" : "linking", false);
	}

	void Runtime::RetryPendingOpen()
	{
		if (_pendingSurfaceOpen && _pendingSurfaceOpen->error) {
			_pendingSurfaceOpen->retryRequested = true;
		}
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
				// Shape was validated synchronously at the ABI boundary; what's
				// left here is precedence (native wins, logged inside).
				store.RegisterSchema(std::move(op.schema), SettingsStore::Source::kNative);
			} else if (store.GetSource(op.modId) == SettingsStore::Source::kNative) {
				store.RemoveMod(op.modId);
			} else {
				REX::WARN("Runtime: UnregisterSettingsSchema('{}') ignored — not a runtime-registered schema", op.modId);
			}
		}
	}

	void Runtime::DrainViewRegistrations()
	{
		auto ids = API::BridgeApi::Get().TakeViewRegistrations();
		if (ids.empty()) {
			return;
		}
		if (!_renderer) {
			// Overlay disabled or never came up: drop loudly rather than
			// queueing forever.
			for (const auto& id : ids) {
				REX::WARN("Runtime: plugin RegisterView('{}') ignored — overlay not running", id);
			}
			return;
		}
		bool catalogChanged = false;
		for (const auto& id : ids) {
			// Idempotent: reloading a live surface (config-listed or a repeat
			// call) would blow away its page state.
			if (_menus.IsRegistered(id)) {
				REX::DEBUG("Runtime: plugin RegisterView('{}') — already a registered surface, left untouched", id);
				continue;
			}
			if (!_renderer->SupportsMultipleViews()) {
				REX::WARN("Runtime: plugin RegisterView('{}') refused — renderer '{}' is single-view",
					id, _renderer->Name());
				continue;
			}
			const auto* m = _views.Find(id);
			if (!m) {
				REX::WARN("Runtime: plugin RegisterView('{}') ignored — no views/{}/manifest.json was discovered at boot (ids are qualified '<author>.<modname>/<view>'; is the view folder installed?)", id, id);
				continue;
			}
			if (!LoadSurface(*m, "via plugin RegisterView")) {
				continue;
			}
			if (m->openOnStart) {
				_menus.Open(id);
			}
			catalogChanged = true;
		}
		if (catalogChanged) {
			ApplyMenuPolicy();     // openOnStart / z-band changes take effect now
			BroadcastViewsData();  // the Mods surface picks the new view up live
		}
	}

	void Runtime::ApplyMenuPolicy()
	{
		if (!_renderer) {
			return;
		}
		// Per-surface hidden + composite z, derived from the band order: HUDs
		// beneath menus; HUDs by `order`, menus by open-stack position.
		for (const auto& layer : _menus.DesiredLayers()) {
			_renderer->SetViewHidden(layer.id, layer.hidden);
			_renderer->SetViewOrder(layer.id, layer.z);
		}
		// Focus follows the top menu; HUD-only => no active view to set.
		const auto active = _menus.ActiveMenu();
		if (active) {
			_renderer->SetActiveView(*active);
		}
		// Capture is the top menu's policy (false for HUD-only => the game keeps
		// input). The runtime writer of _captureInput; the boot-time default is
		// stored from config during Initialize.
		const bool desiredCapture = _menus.DesiredCapture();
		const bool captureChanged = _captureInput.exchange(desiredCapture) != desiredCapture;
		if (captureChanged) {
			// Hardware cursor state belongs to the game window thread. Wake it now;
			// a menu-session focus transfer can otherwise happen before the next
			// WM_INPUT packet and leave the OS pointer hidden for the whole session.
			OverlayInputHook::RequestStateRefresh();
		}

		// Visibility side-effects live here rather than behind a change guard,
		// which would drop the compositor push on the no-change startup path.
		const bool visible = _menus.DesiredVisible();
		const bool wasVisible = _visible.exchange(visible);
		// Interactive menus use real browser focus for the full session so Windows
		// schedules Chromium as foreground work. HUD-only views leave the game
		// focused. Mouse and controller have focus-independent paths for the menu.
		ReconcileNativeFocus();
		if (_compositor) {
			if (visible && !wasVisible) {
				// Closed->open edge: defer the reveal. The compositor redraws
				// its last cached texture every present while visible, so
				// showing it now would flash stale pre-open content for the
				// frames it takes the renderer to deliver queued messages and
				// hand over a post-open frame.
				_revealPending = true;
				_revealFrameReady = false;
			} else {
				if (!visible) {
					_revealPending = false;  // closed while a reveal was still pending
					_revealFrameReady = false;
				}
				if (!_revealPending) {
					_compositor->SetVisible(visible);
				}
			}
		}

		// Open->closed edge: flush the settings write-behind instead of waiting
		// out the window (mcm-design.md §8.1; the shutdown flush is
		// ~SettingsStore).
		if (!visible && wasVisible && _settings) {
			_settings->Store().FlushPersistence();
		}

		// Recenter the virtual cursor on the closed->open edge, else keep its
		// position; either way (re)place it in the active menu so a freshly
		// focused view shows it at the right spot, not its stale origin.
		if (visible) {
			if (!wasVisible) {
				_cursorX = _viewWidth.load() * 0.5f;
				_cursorY = _viewHeight.load() * 0.5f;
			}
			if (active) {
				QueueMouseMove();  // flushed by Tick's once-per-frame move injection
			}
		}
		// ui.visibility keys off the shown view (the focused menu of a visible
		// overlay) changing, not off the overlay's open/close edge: a view switch
		// while the overlay stays up (hub -> panel) is a real show for the new view
		// and a real hide for the old one. Consumers arm whole sessions off this
		// signal, so an edge-only send left hub-opened views permanently "closed".
		// The hide can't render a fade-out (the compositor already hid this frame
		// on the overlay-close path), but the view's JS keeps running while hidden.
		// By overlay close ActiveMenu() is already empty, hence the tracked name.
		if (_bridge) {
			const std::string shown = (visible && active) ? *active : std::string();
			if (shown != _lastShownView) {
				// reason lets views scope per-overlay-visit state to real overlay
				// edges while still seeing focus handoffs: "overlay" = the overlay
				// opened/closed this tick, "focus" = only the focused menu changed.
				const char* reason = (visible == wasVisible) ? "focus" : "overlay";
				if (!_lastShownView.empty()) {
					_bridge->SendToWeb(_lastShownView, "ui.visibility",
						nlohmann::json{ { "visible", false }, { "reason", reason } });
				}
				if (!shown.empty()) {
					_bridge->SendToWeb(shown, "ui.visibility",
						nlohmann::json{ { "visible", true }, { "reason", reason } });
				}
				_lastShownView = shown;
			}
		}
		if (visible != wasVisible) {
			REX::INFO("Runtime: overlay visibility -> {} (capture={})", visible, _captureInput.load());
		}

		BroadcastViewsData();
	}

	void Runtime::ReconcileNativeFocus()
	{
		// Runs from Tick (an off-main worker; proven 2026-07-23). This drives
		// renderer/WebView focus, not engine main-thread state, so the thread it
		// runs on is not load-bearing here.
		// Edge-guarded: the false side posts a game-focus restore to the window
		// thread, and the true side races Chromium's async MoveFocus, so repeat
		// sends would only feed the focus watchdog more churn.
		if (!_renderer) {
			return;
		}
		const auto active = _menus.ActiveMenu();
		// A capturing menu owns native focus for its whole visible session. This
		// reproduces Windows' smooth foreground scheduling without changing GPU
		// priorities. HUD-only views have no active capturing menu and leave the
		// game focused.
		const bool want = _visible.load() && _captureInput.load() && active.has_value();
		if (want == _nativeFocusGranted) {
			return;
		}
		_nativeFocusGranted = want;
		_renderer->SetNativeFocus(want);
	}

	bool Runtime::IsVisible() const
	{
		return _visible.load();
	}

	bool Runtime::SetViewHidden(std::string_view a_id, bool a_hidden)
	{
		// The renderer would silently no-op an unknown id; reject for a clear log.
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
		REX::DEBUG("Runtime: view '{}' hidden -> {}", a_id, a_hidden);
		return true;
	}

	void Runtime::OnViewLoad(std::string_view a_viewId, bool a_failed, std::string_view a_url,
		std::string_view a_description, int a_errorCode)
	{
		const std::string id(a_viewId);
		_viewLoadState[id] = a_failed ? ViewLoadState::Failed : ViewLoadState::Finished;
		// The gamepad-raw and back-owner grants are sticky for a page's lifetime,
		// so a (re)loaded page starts un-granted and re-asserts in its own boot code.
		_gamepadRawViews.erase(id);
		_backOwnerViews.erase(id);
		if (!a_failed) {
			// A healthy load clears the strikes, so a later failure gets the full
			// retry budget again.
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

		// Crash-recovery: schedule a bounded reload with backoff. attempts counts
		// reloads already fired; an exhausted budget means the content is broken,
		// so tear the view down and unregister its surface — otherwise the toggle
		// key / menu.open can re-open an invisible, input-capturing shell.
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
			API::BridgeApi::Get().SetSurfaceLoaded(id, false);
			bool bridgeSurfaceRemains = false;
			for (const auto& manifest : _views.All()) {
				if (manifest.permissions.nativeBridge && _menus.IsRegistered(manifest.id)) {
					bridgeSurfaceRemains = true;
					break;
				}
			}
			if (!bridgeSurfaceRemains) {
				API::BridgeApi::Get().OnBridgeReady(nullptr);
			}
			_viewsSubscribers.erase(id);  // a destroyed view can't receive pushes
			_i18nSubscribers.erase(id);
			_gamepadRawViews.erase(id);   // its sticky gamepad grant dies with it
			_backOwnerViews.erase(id);    // ditto the back-owner grant
			for (const auto& mod : _modules) {
				mod->OnViewDestroyed(id);  // module-held subscriber sets too
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
			_viewLoadState[id] = ViewLoadState::Loading;
			_readyViews.erase(id);
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
		// HUD-only setups have no reload target: dev iteration on HUDs goes
		// through the browser harness (mcm-design.md §12.2).
		const auto active = _menus.ActiveMenu();
		if (!active) {
			REX::DEBUG("Runtime: dev reload — no open menu to reload");
			return;
		}
		const auto* manifest = _views.Find(*active);
		if (!manifest) {
			return;
		}
		REX::DEBUG("Runtime: dev-reloading view '{}' (devReloadKey)", *active);
		// Same pair as crash-recovery: fresh URL load, then restore the
		// output-matched size so it composites 1:1 again.
		_viewLoadState[*active] = ViewLoadState::Loading;
		_readyViews.erase(*active);
		BroadcastViewsData();
		_renderer->LoadView(*manifest);
		_renderer->Resize(_viewWidth.load(), _viewHeight.load());
	}

	nlohmann::json Runtime::BuildViewsData() const
	{
		nlohmann::json views = nlohmann::json::array();
		const auto     active = _menus.ActiveMenu();
		for (const auto& m : _views.All()) {
			// Every discovered manifest is a launchable surface, so list them all.
			// A registered surface carries its live load state; a discovered-but-
			// unregistered one is reported "unloaded" so the Mods launcher can show
			// it as a click-to-load card (the click's menu.open loads it on demand
			// through EnqueueOpenView). A view whose recovery was exhausted stays
			// "failed" — its _viewLoadState entry survives the Unregister, so it is
			// caught below before the registered/unloaded split. hub:false and
			// debugOnly views are still withheld via the hub flag.
			const bool registered = _menus.IsRegistered(m.id);
			const auto state = GetViewLoadState(m.id);
			const char* loadState =
				state == ViewLoadState::Failed   ? "failed" :
				state == ViewLoadState::Finished ? "loaded" :
				registered                       ? "loading" :
				                                   "unloaded";
			views.push_back(nlohmann::json{
				{ "id", m.id },
				{ "title", _localization.Resolve(m.mod,
					"views." + m.id.substr(m.id.find('/') + 1) + ".title", m.title) },
				{ "description", _localization.Resolve(m.mod,
					"views." + m.id.substr(m.id.find('/') + 1) + ".description", m.description) },
				{ "mod", m.mod },
				{ "kind", m.kind == SurfaceKind::Hud ? "hud" : "menu" },
				{ "interactive", m.interactive },
				{ "hub", m.hub && (!m.debugOnly || _config.debugMode) },
				{ "targetVersion", m.targetVersion },
				{ "open", _menus.IsOpen(m.id) },
				{ "focused", active.has_value() && *active == m.id },
				{ "loadState", loadState },
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
		_bridge->SendJsonToWeb(_viewsSubscribers, "views.data", _lastViewsData);
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
			[this] { EnqueueMenuRequest(MenuReq::Back); });
	}

	bool Runtime::OnHostKey(std::uint32_t a_vkCode, bool a_down)
	{
		// Key-rebind capture (armed by settings.captureKey). Grab the next key
		// press and consume it, so pressing the current toggle key (or Esc)
		// rebinds instead of closing the overlay. Only stash the VK here; the
		// apply happens on the main thread in DrainKeyCapture. The matching key-up
		// is swallowed too so it can't leak/route.
		if (_captureArmed.load()) {
			if (a_down) {
				_capturedVk.store(a_vkCode);
				_captureArmed.store(false);
				_captureUpVk = a_vkCode;
			}
			return true;
		}
		const auto captureUpVk = _captureUpVk.load();
		if (captureUpVk != kInvalidKeyCode && a_vkCode == captureUpVk && !a_down) {
			_captureUpVk = kInvalidKeyCode;
			return true;
		}

		// Dev view-reload key (mcm-design.md §12.1; _devReloadKey only resolves in
		// devMode). Window thread: only raise the flag — the reload runs from Tick
		// (DriveDevReload; renderer calls are main-thread). Consumed on both edges
		// like the toggle key, and before hotkey dispatch so a mod binding the same
		// key doesn't also fire. Capture (above) still wins: mid-rebind this key is
		// a binding like any other.
		if (_devReloadKey != kInvalidKeyCode && a_vkCode == _devReloadKey) {
			if (a_down) {
				_devReloadRequested.store(true);
			}
			return true;
		}

		// Hotkey dispatch (mcm-design.md §9): a key-down edge may fire mods'
		// key-typed bindings. The service self-suppresses while the overlay
		// captures input or a rebind is armed (belt and braces — the armed path
		// above already returned); fires queue here on the window thread and
		// deliver from Tick (DrainHotkeys). Does not consume: the game (and the
		// toggle/router path below) still sees the key.
		if (a_down) {
			_hotkeys.OnKeyDown(a_vkCode);
		}

		// Decide consumption before routing: capturing or the toggle key must not
		// reach the game (the toggle press itself is consumed so opening the
		// overlay never also acts in-game).
		const bool consume = IsInputCaptured() || a_vkCode == _toggleKey;
		if (a_down) {
			_input.OnKeyDown(a_vkCode);
		} else {
			_input.OnKeyUp(a_vkCode);
		}
		return consume;
	}


	bool Runtime::OnNativeAcceleratorKey(std::uint32_t a_vkCode, bool a_down)
	{
		const bool frameworkOwned =
			_captureArmed.load() ||
			(_captureUpVk.load() != kInvalidKeyCode && a_vkCode == _captureUpVk.load()) ||
			a_vkCode == _toggleKey ||
			(_devReloadKey != kInvalidKeyCode && a_vkCode == _devReloadKey) ||
			(a_vkCode == 0x1B && IsInputCaptured());
		return frameworkOwned && OnHostKey(a_vkCode, a_down);
	}
	void Runtime::OnHostChar(std::uint32_t a_codepoint)
	{
		if (!IsInputCaptured() || !_renderer) {
			return;
		}
		// Pure text entry; the VK stream handles toggle/focus via OnHostKey.
		_renderer->InjectCharEvent(a_codepoint);
	}

	void Runtime::OnHostMouseAbsolute(int a_clientX, int a_clientY, int a_clientW, int a_clientH)
	{
		if (!IsInputCaptured() || !_renderer || a_clientW <= 0 || a_clientH <= 0) {
			return;
		}
		// The OS pointer moves in window-client space; the view is the same aspect
		// but height-capped (OnOutputResized), so scale through the client size.
		// Uniform scale keeps the pointer and the page's hit-testing aligned at
		// every resolution.
		const auto viewW = static_cast<float>(_viewWidth.load(std::memory_order_relaxed));
		const auto viewH = static_cast<float>(_viewHeight.load(std::memory_order_relaxed));
		_cursorX = std::clamp(static_cast<float>(a_clientX) * viewW / static_cast<float>(a_clientW), 0.0f, viewW - 1.0f);
		_cursorY = std::clamp(static_cast<float>(a_clientY) * viewH / static_cast<float>(a_clientH), 0.0f, viewH - 1.0f);
		QueueMouseMove();
	}

	void Runtime::OnHostMouseDelta(int a_dx, int a_dy)
	{
		if (!IsInputCaptured() || !_renderer) {
			return;
		}
		// Scale raw deltas so a view sweep costs the same physical travel at any
		// resolution; the view tracks the screen, so 1:1 would feel slow when big.
		const auto scale = _cursorScale.load(std::memory_order_relaxed);
		const auto maxX = static_cast<float>(_viewWidth.load(std::memory_order_relaxed) - 1);
		const auto maxY = static_cast<float>(_viewHeight.load(std::memory_order_relaxed) - 1);
		_cursorX = std::clamp(_cursorX + static_cast<float>(a_dx) * scale, 0.0f, maxX);
		_cursorY = std::clamp(_cursorY + static_cast<float>(a_dy) * scale, 0.0f, maxY);
		QueueMouseMove();
	}

	void Runtime::QueueMouseMove()
	{
		// Raw-input packets arrive at the mouse's polling rate (500-1000 Hz);
		// a pipe write per packet is pure overhead when the page samples at
		// display refresh. Last writer wins — only the newest position
		// matters — and Tick flushes at most one InjectMouseMove per frame.
		// Coords are non-negative ints well under 2^31, so the packed value
		// can never equal the all-bits-set no-pending sentinel.
		const auto x = static_cast<std::uint32_t>(static_cast<int>(_cursorX));
		const auto y = static_cast<std::uint32_t>(static_cast<int>(_cursorY));
		_pendingMouseMove.store((static_cast<std::uint64_t>(x) << 32) | y);
		_mouseMovePackets.fetch_add(1, std::memory_order_relaxed);
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
		// Route at the current virtual cursor; the renderer forwards the raw
		// delta to the host's WebView2 WHEEL input, which performs the scroll.
		_renderer->InjectPhysicalMouseWheel(
			static_cast<int>(_cursorX), static_cast<int>(_cursorY), a_wheelDelta);
	}

	void Runtime::ReconcileFocusMenu()
	{
		// Runs from Tick (an off-main render-graph worker; proven 2026-07-23).
		// Drive the engine menu's open state toward the top menu's capture policy
		// — via the fire-and-forget UIMessageQueue (thread-safe), not a direct
		// RE::UI walk; the watchdog below reads the pump's main-thread snapshot for
		// the same reason. Pause is not wired through this menu's flags
		// (the real pause flag, bit 1, would tie pause to capture instead of the
		// per-view pausesGame policy) — sim pause is ReconcileSimPause. Act only
		// on a change, to avoid per-frame queue spam.
		const bool wantOpen = _menus.DesiredCapture();
		if (wantOpen != _focusMenuOpen) {
			_focusMenuOpen = wantOpen;
			_focusMenuMismatchSince = -1.0;  // fresh request: full grace window
			if (wantOpen) {
				FocusMenu::Open();
			} else {
				FocusMenu::Close();
				// One observer summary per overlay session (no-op unless engineInput).
				EngineInput::LogSessionSummary();
				// Gamepad raw-passthrough is not reset here: it is a sticky
				// per-view property (_gamepadRawViews) that survives overlay
				// hide/show. Another menu opening can't inherit it, because
				// DrainEngineInput reads the active view's flag each tick.
			}
			return;
		}

		// Watchdog: the request above is a fire-and-forget UI-queue message, so the
		// engine's admitted state must be checked to converge. A dropped kHide
		// leaves the engine in menu mode with the overlay gone — every control (Esc
		// included) dead until the process is killed (bug report 2026-07-20). A
		// dropped kShow is the milder mirror (game input under a capturing
		// overlay). The grace window covers queue latency (a frame or two) and
		// transition churn: a load-screen stack clear is followed by
		// MenuEventSink's CloseAll within a tick, re-entering the branch above.
		if (!FocusMenu::IsRegistered()) {
			return;
		}
		// Prefer the pump's frame-old snapshot: IsOpenInEngine walks RE::UI's
		// menuArray, and this tick runs on a render-graph worker, not the main
		// thread — a direct walk races engine mutation. The legacy direct read
		// remains only for the pump-not-installed case (accepting the old race
		// rather than losing the watchdog, whose absence is an input-death bug).
		const auto engineOpenSnap = MainThreadMenuPump::FocusMenuOpenSnapshot();
		const bool engineOpen = engineOpenSnap ? *engineOpenSnap : FocusMenu::IsOpenInEngine();
		if (engineOpen == wantOpen) {
			_focusMenuMismatchSince = -1.0;
			return;
		}
		constexpr double kHealSeconds = 1.0;
		if (_focusMenuMismatchSince < 0.0) {
			_focusMenuMismatchSince = _uptime;
			return;
		}
		if (_uptime - _focusMenuMismatchSince < kHealSeconds) {
			return;
		}
		REX::WARN("FocusMenu: engine admitted state diverged from requested (want {}, engine {}) "
				  "for {:.1f}s; re-sending {} (watchdog)",
			wantOpen ? "open" : "closed", wantOpen ? "closed" : "open",
			_uptime - _focusMenuMismatchSince, wantOpen ? "kShow" : "kHide");
		_focusMenuMismatchSince = -1.0;  // re-arm: another full window before the next retry
		if (wantOpen) {
			FocusMenu::Open();
		} else {
			FocusMenu::Close();
		}
	}

	void Runtime::ReconcileSimPause()
	{
		// Unconditional: the sim pause needs no engine menu
		// (UI::ModifyMenuPauseCounter; see input/SimPause), so it is not gated on
		// config.focusMenu. Driven by the top menu's manifest pausesGame (default
		// true for menus). Edge-triggered inside Apply, which marshals the counter
		// touch onto the main thread (Tick runs off-main; proven 2026-07-23).
		SimPause::Apply(_menus.DesiredPause());
	}

	void Runtime::DrainEngineInput(double a_deltaSeconds)
	{
		if (!_renderer) {
			return;
		}
		const bool captured = IsInputCaptured();
		const auto active = _menus.ActiveMenu();
		// Raw mode is the active view's sticky flag — per view, so menu switches
		// can't leak one page's grant to another. The EngineInput global mirrors
		// it, keeping the mode-flip log in one place.
		const bool raw = active && _gamepadRawViews.contains(*active);
		EngineInput::SetRawMode(raw);
		// While capturing, the receiver thunks consume gamepad events after
		// recording them (status=kStop): the ControlLayer disable flags do not
		// gate thumbstick movement, so without this the player walks around
		// under the open overlay. Tracks capture, not visibility — a live HUD
		// (no capture) must leave the pad with the game.
		EngineInput::SetConsumeGamepad(captured);

		// Discrete down+up tap: a missed release can't leave a stuck key.
		const auto tap = [this](std::uint32_t a_vk) {
			_renderer->InjectKeyEvent(a_vk, true);
			_renderer->InjectKeyEvent(a_vk, false);
		};

		const auto routeButtonEdge = [&](const EngineInput::GamepadButtonEdge& e) {
			// Raw event for every edge — a page may own gamepad handling. Per-kind
			// nesting keeps extensions (e.g. a `pad` index) off the payload root.
			if (_bridge && active) {
				_bridge->SendToWeb(*active, "ui.gamepad",
					nlohmann::json{ { "kind", "button" },
						{ "button", { { "id", e.idCode }, { "down", e.down } } } });
			}
			if (raw || !e.down) {
				return;  // raw mode = page owns it; else act on the press edge only
			}
			switch (e.idCode) {
			case XInputButton::kDPadUp:    tap(0x26); break;  // VK_UP
			case XInputButton::kDPadDown:  tap(0x28); break;  // VK_DOWN
			case XInputButton::kDPadLeft:  tap(0x25); break;  // VK_LEFT
			case XInputButton::kDPadRight: tap(0x27); break;  // VK_RIGHT
			case XInputButton::kA:         tap(0x0D); break;  // VK_RETURN — activate
			case XInputButton::kB:         EnqueueMenuRequest(MenuReq::Back); break;  // back — delegate (osfui.handleBack) or close
			default: break;  // shoulders/thumbs/Start/Back -> raw event only
			}
		};

		// Starfield's Windows.Gaming.Input dispatch stops when WebView2 owns
		// foreground focus. Poll XInput only during that capturing interval and
		// keep draining the engine queue so no stale edges are replayed later.
		const bool directPad = captured && _nativeFocusGranted;
		XInputPoller::State directState{};
		EngineInput::GamepadButtonEdge e;
		if (directPad) {
			while (EngineInput::PollGamepadButton(e)) {}
			directState = XInputPoller::Poll();
			if (!_directPadActive) {
				// Baseline only: a held menu-open button must not activate the page.
				_directPadActive = true;
				_directPadButtons = directState.buttons;
			} else {
				const auto changed = _directPadButtons ^ directState.buttons;
				constexpr std::uint32_t masks[] = {
					XInputButton::kDPadUp, XInputButton::kDPadDown,
					XInputButton::kDPadLeft, XInputButton::kDPadRight,
					XInputButton::kStart, XInputButton::kBack,
					XInputButton::kLThumb, XInputButton::kRThumb,
					XInputButton::kLShoulder, XInputButton::kRShoulder,
					XInputButton::kA, XInputButton::kB,
					XInputButton::kX, XInputButton::kY,
				};
				for (const auto mask : masks) {
					if ((changed & mask) != 0) {
						routeButtonEdge({ mask, (directState.buttons & mask) != 0 });
					}
				}
				_directPadButtons = directState.buttons;
			}
		} else {
			_directPadActive = false;
			_directPadButtons = 0;
			while (EngineInput::PollGamepadButton(e)) {
				if (captured) {
					routeButtonEdge(e);
				}
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

		const auto s = directPad ?
			EngineInput::GamepadSticks{ directState.lx, directState.ly, directState.rx, directState.ry } :
			EngineInput::GetSticks();
		constexpr float       kDeadzone = 0.25f;

		// Raw stick events, throttled to meaningful change, so a page can drive
		// e.g. camera orbit off the raw values.
		if (_bridge && active) {
			const float cur[4] = { s.lx, s.ly, s.rx, s.ry };
			bool        changed = false;
			for (int i = 0; i < 4; ++i) {
				changed = changed || std::fabs(cur[i] - _padLastSentSticks[i]) > 0.04f;
			}
			if (changed) {
				// Nested like the button case; triggers extend as axes.lt/rt.
				_bridge->SendToWeb(*active, "ui.gamepad",
					nlohmann::json{ { "kind", "stick" },
						{ "axes", { { "lx", s.lx }, { "ly", s.ly }, { "rx", s.rx }, { "ry", s.ry } } } });
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

		// Right stick Y -> scroll. Fractional notches accumulate for
		// framerate-independent scrolling; +y (stick up) = wheel up.
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
		// Tick runs on an off-main render-graph worker (proven 2026-07-23), but the
		// input-enable layer (BSInputEnableManager) is main-thread-owned, so
		// ControlLayer::Apply marshals the engage/release onto the main thread via
		// BSService::TaskQueue (edges detected internally; no-ops until the manager
		// exists). This is the only gate that stops gamepad/XInput, so it tracks
		// capture (not pause), or a gamepad drives the game underneath a capturing
		// menu. A live HUD (no capture) leaves controls enabled.
		ControlLayer::Apply(_menus.DesiredCapture());
	}

	void Runtime::BuildModules()
	{
		// Settings: schemas ship read-only under <data>/settings/*.json; values
		// persist per-mod under <data>/settings/values — in the Data tree, not
		// Documents, because under MO2 the write is VFS-captured (Overwrite), so
		// settings are per-profile, travel with instance backups, and sit next to
		// the mod (MCM-Helper precedent; mcm-design.md §8.1).
		const auto schemaDir = Paths::DataDir() / "settings";
		const auto valuesDir = schemaDir / "values";
		auto settings = std::make_unique<SettingsModule>(schemaDir, valuesDir,
			[this](std::string_view a_mod, std::string_view a_key, const nlohmann::json& a_value) {
				OnSettingChanged(a_mod, a_key, a_value);
			});
		_settings = settings.get();  // core needs schema facts (e.g. key-capture gating)
		_settings->Store().SetTextResolver([this](std::string_view a_mod, std::string_view a_address, std::string_view a_english) {
			return _localization.Resolve(a_mod, a_address, a_english);
		});

		// ABI feed (mcm-design.md §8.2): every committed value — including the
		// OnStart NotifyAll replay below and the per-mod replay after an
		// incremental RegisterSchema — lands in the any-thread mirror the C ABI
		// typed getters read, then queues for SubscribeSettings consumers (drained
		// on the main thread by BridgeApi::PumpMainThread). Mirror first: a
		// subscribe replay snapshots the mirror, so it must never lag the queued
		// event. Registry shape changes rebuild the mirror from the store document
		// so a removed mod's values stop resolving.
		auto& store = _settings->Store();
		store.AddChangeListener([](std::string_view a_mod, std::string_view a_key, const nlohmann::json& a_value) {
			auto& api = API::BridgeApi::Get();
			api.Mirror().Update(a_mod, a_key, a_value);
			api.Subscriptions().OnChanged(a_mod, a_key, a_value);
			// Papyrus change callbacks (mcm-design.md §8.4), after the mirror
			// update: the dispatched script call reads current values through the
			// mirror-backed getters, so the mirror must never lag it.
			API::Papyrus::OnSettingChanged(a_mod, a_key);
		});
		store.AddRegistryListener([this] {
			if (_settings) {  // teardown guard (_settings nulls before modules die)
				API::BridgeApi::Get().Mirror().Rebuild(_settings->Store().DataView());
			}
		});

		// HotkeyService (mcm-design.md §9): every key-typed setting is a live
		// binding. The registry rebuilds on any key-typed commit (web, ABI or
		// reset) and on registry shape change; the store's conflict grouping shares
		// this key-name resolution, so the store stays input-agnostic. Suppression
		// reads the same capture state OnHostKey consults, so a press while the
		// user types in a settings field or mid-rebind cannot fire a hotkey.
		store.SetKeyNameResolver(ResolveKeyName);

		// Vanilla hotkeys (mcm-design.md §9) are not loaded here: the
		// osfui.vanillaKeyConflicts setting is MCM-owned, so the OnStart NotifyAll
		// replay drives ApplyVanillaKeyConflicts with the persisted value (default
		// on → loads then; off → never pays the parse).

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
		// settings.set, so the store persists and OnSettingChanged re-resolves.
		nlohmann::json payload{
			{ "mod", _captureMod },
			{ "key", _captureKey },
			{ "name", name },
			{ "cancelled", cancelled },
		};
		// Live-warn during capture (mcm-design.md §9): which other key-typed
		// settings already sit on this key, so the UI warns before the view
		// commits. The store still holds this setting's old binding (the commit is
		// the view's echo), so exclude self. Informational, never blocking.
		if (!cancelled && _settings) {
			if (auto conflicts = _settings->Store().ConflictsFor(vk, _captureMod, _captureKey); !conflicts.empty()) {
				payload["conflicts"] = std::move(conflicts);
			}
		}
		// Deferred reply: echo the arming request's id so the view's
		// osfui.request("settings.captureKey", ...) promise settles with this.
		_bridge->SendToWeb(_captureView, "settings.captured", payload, _captureRequestId);
		REX::DEBUG("Runtime: key capture -> {} (VK {:#04x}) ({}.{})",
			cancelled ? "(cancelled)" : name, vk, _captureMod, _captureKey);
		_captureView.clear();
		_captureMod.clear();
		_captureKey.clear();
		_captureRequestId.clear();
	}

	void Runtime::DrainHotkeys()
	{
		// Gameplay gate (mcm-design.md §9): a press while a game menu is up
		// (PauseMenu, inventory, dialogue, main menu, ...) must not fire. Checked
		// at delivery on the game thread via the engine's menu-mode discriminator
		// (MenuMode.h), lazily so idle ticks never touch RE::UI. Gated presses are
		// dropped, not deferred — replaying them on menu close would be worse.
		std::optional<bool> inGameMenu;
		_hotkeys.Drain([this, &inGameMenu](const std::string& a_mod, const std::string& a_key) {
			if (!inGameMenu) {
				// Pump snapshot first: AnyGameMenuOpen walks RE::UI's menuArray
				// and this tick runs on a render-graph worker (see the focus
				// watchdog note). Direct walk only when the pump is absent.
				const auto snap = MainThreadMenuPump::AnyGameMenuOpenSnapshot();
				inGameMenu = snap ? *snap : MenuMode::AnyGameMenuOpen();
			}
			if (*inGameMenu) {
				// INFO on purpose: rare (a bound key inside a menu/console), and the
				// decisive triage line for "my hotkey (didn't) fire" reports.
				REX::DEBUG("Runtime: hotkey {}.{} dropped (game menu open)", a_mod, a_key);
				return;
			}
			// Delivery channels (mcm-design.md §9): C ABI subscribers (queued
			// here, invoked unlocked by BridgeApi::PumpMainThread later this
			// tick) and the web `ui.hotkey` push to settings subscribers.
			API::BridgeApi::Get().Hotkeys().OnFired(a_mod, a_key);
			if (_settings) {
				_settings->PushHotkey(a_mod, a_key);
			}
			// Third channel (mcm-design.md §8.4): registered Papyrus callbacks,
			// queued onto the VM's async call stack.
			API::Papyrus::OnHotkey(a_mod, a_key);
			REX::DEBUG("Runtime: hotkey fired for {}.{}", a_mod, a_key);
		});
	}

	void Runtime::RegisterPlatformCommands(MessageBridge& a_bridge)
	{
		// The platform owns only window/diagnostic commands. Features register
		// their own; there is no generic "call native" escape hatch.
		a_bridge.RegisterCommand("close", [this](const nlohmann::json&, MessageBridge& a_b) {
			if (a_b.CurrentSource() == kHandoffViewId && CancelPendingOpen()) {
				ApplyMenuPolicy();
				return;
			}
			// Dismiss the calling surface. Closing the last open menu empties the
			// stack, so the overlay hides; a coexisting live HUD stays up.
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
		// Open/close a surface by id (defaults to the calling view). menu.* and
		// hud.* are aliases: a surface's kind is fixed by its manifest, not by the
		// command used.
		const auto surfaceOpen = [this](const nlohmann::json& a_p, MessageBridge& a_b) {
			std::string id = Json::GetString(a_p, "view", "");
			if (id.empty()) {
				id = std::string(a_b.CurrentSource());
			}
			if (!_views.Find(id)) {
				REX::WARN("Runtime: menu.open/hud.show refused — '{}' was not discovered", id);
				a_b.SendResult(false, "unknown-view", "view was not discovered");
				return;
			}
			// Use the same snapshot/load/pump/open path as native RequestMenu so a
			// discovered surface is created while hidden on the next tick.
			EnqueueOpenView(std::move(id));
		};
		const auto surfaceClose = [this](const nlohmann::json& a_p, MessageBridge& a_b) {
			std::string id = Json::GetString(a_p, "view", "");
			if (id.empty()) {
				id = std::string(a_b.CurrentSource());
			}
			bool cancelled = false;
			if (_pendingSurfaceOpen &&
				(_pendingSurfaceOpen->target == id || id == kHandoffViewId)) {
				cancelled = CancelPendingOpen();
			}
			if (_menus.Close(id)) {
				ApplyMenuPolicy();
			} else if (cancelled) {
				ApplyMenuPolicy();
			} else if (!_menus.IsRegistered(id)) {
				a_b.SendResult(false, "unknown-view", "not a registered surface");
			}
			// Already closed = desired state reached: the auto ui.result acks it.
		};
		a_bridge.RegisterCommand("menu.open", surfaceOpen);
		a_bridge.RegisterCommand("menu.close", surfaceClose);
		a_bridge.RegisterCommand("hud.show", surfaceOpen);
		a_bridge.RegisterCommand("hud.hide", surfaceClose);
		a_bridge.RegisterCommand("view.ready", [this](const nlohmann::json&, MessageBridge& a_b) {
			const std::string source(a_b.CurrentSource());
			const auto* manifest = _views.Find(source);
			if (!manifest || !manifest->permissions.nativeBridge) {
				a_b.SendResult(false, "forbidden", "view.ready requires nativeBridge");
				return;
			}
			_readyViews.insert(source);
			REX::DEBUG("Runtime: view '{}' declared meaningful readiness", source);
		});
		a_bridge.RegisterCommand("osfui.handoffRetry", [this](const nlohmann::json&, MessageBridge& a_b) {
			if (a_b.CurrentSource() != kHandoffViewId) {
				a_b.SendResult(false, "forbidden", "platform handoff command");
				return;
			}
			RetryPendingOpen();
		});
		a_bridge.RegisterCommand("setViewHidden", [this](const nlohmann::json& a_p, MessageBridge& a_b) {
			// Show/hide one loaded view by id, independent of the overlay toggle.
			// Omitting "view" targets the calling view (self-hide).
			std::string id = Json::GetString(a_p, "view", "");
			if (id.empty()) {
				id = std::string(a_b.CurrentSource());
			}
			if (!SetViewHidden(id, Json::GetBool(a_p, "hidden", false))) {
				a_b.SendResult(false, "unknown-view", "not a loaded view");
			}
		});
		// Catalog of discovered surfaces (bridge 0.2), loaded or not. Replies with
		// `views.data` and subscribes the caller: any later open/close/focus/load-state change
		// re-sends the catalog, so it stays current without polling.
		a_bridge.RegisterCommand("views.get", [this](const nlohmann::json&, MessageBridge& a_b) {
			const auto payload = BuildViewsData();
			_viewsSubscribers.insert(std::string(a_b.CurrentSource()));
			_lastViewsData = payload.dump();
			a_b.SendToWeb("views.data", payload);
		});
		// A custom view supplies inline English to osfui.t(address, english); this
		// returns only active-locale overrides for its mod domain. The caller
		// subscribes so a live language change replaces the catalog.
		a_bridge.RegisterCommand("i18n.get", [this](const nlohmann::json& a_p, MessageBridge& a_b) {
			const std::string source(a_b.CurrentSource());
			const auto slash = source.find('/');
			const std::string ownMod = slash == std::string::npos ? source : source.substr(0, slash);
			std::string mod = Json::GetString(a_p, "mod", ownMod);
			if (!Ids::IsAcceptedModId(mod)) {
				a_b.SendResult(false, "invalid-mod", "invalid localization mod id");
				return;
			}
			_i18nSubscribers[source] = mod;
			a_b.SendToWeb("i18n.data", nlohmann::json{
				{ "mod", mod }, { "locale", _localization.Locale() },
				{ "strings", _localization.CatalogFor(mod) },
			});
		});
		// Arm key-rebind capture: the next key press is grabbed by OnHostKey and
		// reported back via `settings.captured`. Any schema-declared `type:"key"`
		// setting is rebindable — the schema gates the capture, not an allowlist.
		// Main thread; OnHostKey (window thread) reads the armed flag.
		a_bridge.RegisterCommand("settings.captureKey", [this](const nlohmann::json& a_p, MessageBridge& a_b) {
			const std::string mod = Json::GetString(a_p, "mod", "");
			const std::string key = Json::GetString(a_p, "key", "");
			// One capture at a time: a second arm while one is live is refused
			// visibly rather than silently clobbering the first view's pending
			// capture.
			if (_captureArmed.load()) {
				REX::WARN("Runtime: settings.captureKey rejected — a capture is already in progress ({}.{})",
					_captureMod, _captureKey);
				a_b.SendResult(false, "capture-busy", "a key capture is already in progress");
				return;
			}
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
			// Correlation across the async gap: the eventual settings.captured
			// echoes the arming request's id. DeferResult suppresses the auto
			// ui.result — arming is not the outcome.
			_captureRequestId = std::string(a_b.CurrentRequestId());
			a_b.DeferResult();
			_captureArmed.store(true);
			REX::DEBUG("Runtime: armed key capture for {}.{} (from view '{}')", mod, key, _captureView);
		});
		// Fire an action at the owning mod's Papyrus scripts
		// (OSFUI.RegisterForViewActions). The mod id comes from the source view id,
		// never the payload, so a view cannot fire actions into another mod's
		// callbacks. Fire-and-forget: no reply payload; a requestId still gets the
		// auto ui.result ack, meaning "queued to the VM", not "handled".
		a_bridge.RegisterCommand("ui.action", [](const nlohmann::json& a_p, MessageBridge& a_b) {
			const std::string source(a_b.CurrentSource());
			const auto        slash = source.find('/');
			const std::string mod = slash == std::string::npos ? source : source.substr(0, slash);
			const std::string action = Json::GetString(a_p, "action", "");
			if (action.empty()) {
				REX::WARN("Runtime: ui.action from '{}' ignored — empty 'action'", source);
				a_b.SendResult(false, "invalid-action", "ui.action requires a non-empty 'action' string");
				return;
			}
			// `args` (protocol 1.3): a string list delivered to args-list
			// registrants as a Papyrus string[]. Non-string elements are coerced
			// so a view can send `args: [1, 7]` without stringifying. When absent
			// (or not an array) fall back to the legacy scalar `arg` as a single
			// element, so an unmigrated view keeps working unchanged.
			std::vector<std::string> args;
			if (const auto it = a_p.find("args"); it != a_p.end() && it->is_array()) {
				args.reserve(it->size());
				for (const auto& e : *it) {
					if (e.is_string()) {
						args.push_back(e.get<std::string>());
					} else if (e.is_number_integer()) {
						args.push_back(std::to_string(e.get<std::int64_t>()));
					} else if (e.is_number()) {
						args.push_back(std::to_string(e.get<double>()));
					} else if (e.is_boolean()) {
						args.emplace_back(e.get<bool>() ? "true" : "false");
					} else {
						args.emplace_back();  // null/object/array element -> ""
					}
				}
			} else {
				args.push_back(Json::GetString(a_p, "arg", ""));
			}
			API::Papyrus::OnViewAction(mod, action, args);
		});
		a_bridge.RegisterCommand("log", [](const nlohmann::json& a_p, MessageBridge&) {
			// Untrusted content: bound the length so JS cannot flood the log.
			REX::DEBUG("MessageBridge: [web] {}", Json::GetString(a_p, "text", "").substr(0, 512));
		});
		a_bridge.RegisterCommand("ping", [](const nlohmann::json&, MessageBridge& a_b) {
			a_b.SendToWeb("runtime.pong", nlohmann::json::object());
		});
		a_bridge.RegisterCommand("osfui.gamepadRaw", [this](const nlohmann::json& a_p, MessageBridge& a_b) {
			// A page that wants to own the gamepad (e.g. stick-driven camera orbit)
			// sets this to suppress the default nav/scroll mapping and handle raw
			// `ui.gamepad` events itself. Sticky per view: survives overlay
			// hide/show, clears on page reload or view destroy. DrainEngineInput
			// applies the active view's flag each tick.
			const std::string src(a_b.CurrentSource());
			if (src.empty()) {
				a_b.SendResult(false, "unknown-view", "no source view");
				return;
			}
			if (Json::GetBool(a_p, "raw", false)) {
				_gamepadRawViews.insert(src);
			} else {
				_gamepadRawViews.erase(src);
			}
		});
		// Compatibility no-op: pre-session-focus views may still send this
		// experimental command. Interactive menus already own focus throughout,
		// so accepting it avoids an unknown-command break without changing policy.
		a_bridge.RegisterCommand("osfui.textFocus",
			[](const nlohmann::json&, MessageBridge&) {});
		a_bridge.RegisterCommand("osfui.openModPage", [](const nlohmann::json&, MessageBridge& a_b) {
			// "Update OSF UI" affordances in views (e.g. OSF Animation's status-line
			// UPDATE badge): open OSF UI's own Nexus page in the SYSTEM browser —
			// the overlay itself must never navigate, and the URL is a compile-time
			// constant precisely so page content cannot steer the shell (the
			// payload carries nothing). Behind a fullscreen game the browser opens
			// unfocused; alt-tab surfaces it.
			if (Platform::OpenSystemBrowser(kNexusPageURLW)) {
				REX::DEBUG("Runtime: osfui.openModPage -> {}", kNexusPageURL);
			} else {
				REX::WARN("Runtime: osfui.openModPage — the shell refused to open {}", kNexusPageURL);
				a_b.SendResult(false, "shell-failed", "could not open the system browser");
			}
		});
		a_bridge.RegisterCommand("osfui.handleBack", [this](const nlohmann::json& a_p, MessageBridge& a_b) {
			// A page that owns back navigation (e.g. a sub-menu whose Esc should
			// return to the hub, not dismiss the overlay) sets this; while it is
			// the active menu, Esc / pad-B arrive as a synthetic Escape
			// keydown/keyup instead of closing the top menu. Same lifecycle as
			// osfui.gamepadRaw. The toggle key still closes natively, so this
			// cannot strand the user.
			const std::string src(a_b.CurrentSource());
			if (src.empty()) {
				a_b.SendResult(false, "unknown-view", "no source view");
				return;
			}
			if (Json::GetBool(a_p, "handle", false)) {
				_backOwnerViews.insert(src);
			} else {
				_backOwnerViews.erase(src);
			}
		});

		// Read-only game data: the in-game calendar. Bridge handlers dispatch from
		// Tick, which runs on an off-main worker (proven 2026-07-23) — but these
		// are read-only accessors on the long-lived Calendar singleton, so the
		// worst case is a momentarily stale field, not corruption. (Contrast the
		// pause/cursor/input reconcilers, which MUTATE engine state and so marshal
		// to the main thread via BSService::TaskQueue.)
		a_bridge.RegisterCommand("game.get", [](const nlohmann::json&, MessageBridge& a_b) {
			nlohmann::json calendar = nlohmann::json::object();
			if (const auto* cal = RE::Calendar::GetSingleton()) {
				calendar["available"] = true;
				calendar["day"] = cal->GetDay();
				calendar["month"] = cal->GetMonth();
				calendar["year"] = cal->GetYear();
				calendar["hour"] = cal->GetHour();
				calendar["daysPassed"] = cal->GetDaysPassedExact();
			} else {
				calendar["available"] = false;
			}
			a_b.SendToWeb("game.data", nlohmann::json{ { "calendar", std::move(calendar) } });
		});
	}

	void Runtime::OnSettingChanged(std::string_view a_modId, std::string_view a_key, const nlohmann::json& a_value)
	{
		// Only the framework's own knobs (mod "osfui"); other mods' settings are
		// theirs to react to. Invoked from Tick (an off-main worker; proven
		// 2026-07-23) as settings commands dispatch, plus once per value at startup
		// via NotifyAll, so persisted choices apply on boot.
		if (a_modId != "osfui") {
			return;
		}
		// Toggle key rebind: re-resolve and re-apply to the input router. An
		// unresolvable name keeps the working key rather than disabling the toggle.
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
		// Pause-menu entry (MCM-owned). The Scaleform inject runs per pause-menu
		// open (MainThreadMenuPump gates Reconcile on this flag), so the change
		// applies the next time the menu opens.
		else if (a_key == "pauseMenuEntry" && a_value.is_boolean()) {
			_config.pauseMenuEntry = a_value.get<bool>();
			PauseMenuEntry::SetEnabled(_config.pauseMenuEntry);
			REX::DEBUG("Runtime: setting osfui.pauseMenuEntry -> {} (applies the next time the pause menu opens)", _config.pauseMenuEntry);
		}
		// Vanilla key-conflict data (MCM-owned). Lazy build / clear.
		else if (a_key == "vanillaKeyConflicts" && a_value.is_boolean()) {
			_config.vanillaKeyConflicts = a_value.get<bool>();
			ApplyVanillaKeyConflicts(_config.vanillaKeyConflicts);
		}
		else if (a_key == "renderStats" && a_value.is_boolean()) {
			_renderStatsEnabled = a_value.get<bool>();
			_renderStatsHaveBaseline = false;
			if (_compositor) {
				_compositor->SetRenderStatsEnabled(_renderStatsEnabled);
			}
			if (_renderer) {
				for (const auto& manifest : _views.All()) {
					if (_menus.IsRegistered(manifest.id)) {
						_renderer->SetRenderStats(manifest.id, _renderStatsEnabled);
					}
				}
			}
			REX::DEBUG("Runtime: setting osfui.renderStats -> {} for all views",
				_renderStatsEnabled);
		}
		else if (a_key == "debugMode" && a_value.is_boolean()) {
			_config.debugMode = a_value.get<bool>();
			BroadcastViewsData();  // debugOnly views appear/leave the mod menu live
			REX::DEBUG("Runtime: setting osfui.debugMode -> {} (developer views {} in the mod menu)",
				_config.debugMode, _config.debugMode ? "shown" : "hidden");
		}
		else if (a_key == "language" && a_value.is_string()) {
			const auto requested = a_value.get<std::string>();
			const auto documents = Platform::GetDocumentsPath();
			const auto locale = requested == "auto"
				? LocalizationService::DetectGameLocale(documents.empty() ? std::filesystem::path{} : documents / "My Games" / "Starfield")
				: LocalizationService::NormalizeLocale(requested);
			if (_localization.SetLocale(locale)) {
				RefreshLocalizedData();
			}
		}
	}

	void Runtime::RefreshLocalizedData()
	{
		PauseMenuEntry::Configure(
			_localization.Resolve("osfui", "chrome.pauseMenuEntry", _config.pauseMenuEntryLabel),
			_config.pauseMenuEntryView);
		if (_settings) {
			_settings->Store().InvalidateLocalizedData();
			// Rebuild authored game labels under the new locale.
			if (_config.vanillaKeyConflicts) {
				_vanillaKeysApplied = false;
				ApplyVanillaKeyConflicts(true);
			} else {
				_settings->BroadcastData();
			}
		}
		BroadcastViewsData();
		if (_bridge) {
			for (const auto& [view, mod] : _i18nSubscribers) {
				_bridge->SendToWeb(view, "i18n.data", nlohmann::json{
					{ "mod", mod }, { "locale", _localization.Locale() },
					{ "strings", _localization.CatalogFor(mod) },
				});
			}
		}
	}

	void Runtime::ApplyVanillaKeyConflicts(bool a_enabled)
	{
		if (!_settings || a_enabled == _vanillaKeysApplied) {
			return;  // no store, or already in the requested state
		}
		_vanillaKeysApplied = a_enabled;
		auto& store = _settings->Store();
		if (!a_enabled) {
			store.SetVanillaKeys({});
			REX::DEBUG("Runtime: vanilla key-conflict data disabled");
		} else {
			// The game's own bindings join the conflict grouping as "@game"
			// pseudo-entries (mcm-design.md §9; no engine RE). Defaults come from
			// the curated shipped table — the engine bakes its defaults into the
			// executable and no controlmap ships in the archives. The controlmap
			// text files the engine honors overlay it (mod-provided Data override,
			// then the user's in-game remaps), then the user's additive
			// vanillakeys.user.json: fixes survive updates while untouched rows
			// keep upstream corrections.
			VanillaKeys vanilla;
			if (vanilla.LoadDefaults(Paths::DataDir() / "vanillakeys.json", ResolveKeyName)) {
				const auto scanToVk = [](std::uint32_t a_sc) { return Platform::DirectInputScanToVk(a_sc); };
				// DataDir = <Data>/SFSE/Plugins/OSFUI; under MO2 the module
				// path is virtualized, so this resolves through the VFS too.
				const auto gameData = Paths::DataDir().parent_path().parent_path().parent_path();
				vanilla.OverlayControlMap(gameData / "Interface" / "Controls" / "PC" / "ControlMap.txt", scanToVk);
				const auto docs = Platform::GetDocumentsPath();
				if (!docs.empty()) {
					vanilla.OverlayControlMap(docs / "My Games" / "Starfield" / "ControlMap_Custom.txt", scanToVk);
					vanilla.OverlayUserFile(docs / "My Games" / "Starfield" / "OSFUI" / "vanillakeys.user.json", ResolveKeyName);
				}
				std::vector<SettingsStore::VanillaKey> keys;
				for (const auto& b : vanilla.Bindings()) {
					if (b.vk != 0) {
						// Name resolved after the overlays: a rebound event
						// displays its live key, not the curated default's.
						const auto label = _localization.Resolve("osfui", "gameBindings." + b.event + ".label", b.label);
						const auto owner = _localization.Resolve("osfui", "gameBindings.owner", "Starfield");
						keys.push_back({ b.event, owner + " (" + label + ")", b.vk, KeyName(b.vk) });
					}
				}
				store.SetVanillaKeys(std::move(keys));
			}
		}
		// The conflict annotations live inside the settings document, so re-sync
		// any open view (no-op with no subscribers, e.g. at boot).
		_settings->BroadcastData();
	}

	void Runtime::OnOutputResized(std::uint32_t a_width, std::uint32_t a_height)
	{
		if (a_width == 0 || a_height == 0 || !_renderer) {
			return;
		}
		// Match the view's aspect to the screen, height-capped so rasterization
		// stays bounded on 4K+ (the page is responsive, so any size lays out).
		// Equal aspect makes the compositor's fill-the-backbuffer draw a uniform
		// scale, i.e. no distortion.
		constexpr std::uint32_t kMaxViewHeight = 1440;
		const auto viewHeight = (std::min)(a_height, kMaxViewHeight);
		const auto viewWidth = static_cast<std::uint32_t>(
			std::lround(static_cast<double>(a_width) * viewHeight / a_height));

		if (viewWidth == _viewWidth.load() && viewHeight == _viewHeight.load()) {
			return;
		}

		_viewWidth.store(viewWidth);
		_viewHeight.store(viewHeight);
		// Keep cursor speed consistent across resolutions: ~1920 counts sweep the
		// view width regardless of view size.
		_cursorScale.store((std::max)(1.0f, static_cast<float>(viewWidth) / 1920.0f));
		_renderer->Resize(viewWidth, viewHeight);
		REX::DEBUG("Runtime: output {}x{} -> view resized to {}x{} (aspect-correct)",
			a_width, a_height, viewWidth, viewHeight);
	}

	void Runtime::SubmitFrameIfVisible()
	{
		if (!_initialized || !IsVisible() || !_renderer || !_compositor) {
			return;
		}
		if (const auto frame = _renderer->Render()) {
			if (_revealPending) {
				if (frame->frameIndex != _lastSubmittedFrame) {
					_lastSubmittedFrame = frame->frameIndex;
					_compositor->Submit(*frame);  // also arms lazy present-hook setup
					_revealFrameReady = true;
				}
				if (!_revealFrameReady) {
					// Still the frame from before the open; the renderer
					// republishes under a new serial once the (re)shown view is
					// presentable (messages delivered), so hold the reveal.
					return;
				}
				if (!_compositor->IsOutputSizeKnown()) {
					// D3D12 learns the swapchain size from Present. Keep the first
					// manifest-sized texture hidden while that callback arrives.
					return;
				}
				if (frame->width != _viewWidth.load() || frame->height != _viewHeight.load()) {
					// The output callback requested a resize, but the host has not
					// painted the correctly sized replacement yet.
					return;
				}
				_revealPending = false;
				_revealFrameReady = false;
				_compositor->SetVisible(true);  // the cached frame is fresh and output-sized
				return;
			}
			_lastSubmittedFrame = frame->frameIndex;
			_compositor->Submit(*frame);
		}
	}

	void Runtime::UpdateRenderDiagnostics()
	{
		if (!_renderStatsEnabled || !IsVisible() || !_renderer || !_compositor) {
			_renderStatsHaveBaseline = false;
			return;
		}

		const auto current = _compositor->GetRenderStats();
		if (!_renderStatsHaveBaseline) {
			_renderStatsBaseline = current;
			_renderStatsLastSampleAt = _uptime;
			_renderStatsHaveBaseline = true;
			return;
		}
		const auto elapsed = _uptime - _renderStatsLastSampleAt;
		if (elapsed < 2.0) return;

		const auto delta = [](const std::uint64_t a_now, const std::uint64_t a_before) {
			return a_now >= a_before ? a_now - a_before : a_now;
		};
		const auto presents = delta(current.presents, _renderStatsBaseline.presents);
		const auto draws = delta(current.draws, _renderStatsBaseline.draws);
		const auto fresh = delta(current.freshFrames, _renderStatsBaseline.freshFrames);
		const auto reused = delta(current.reusedDraws, _renderStatsBaseline.reusedDraws);
		const auto submits = delta(current.submits, _renderStatsBaseline.submits);
		const auto waits = delta(current.busyWaits, _renderStatsBaseline.busyWaits);
		const auto dropped = delta(current.droppedBusy, _renderStatsBaseline.droppedBusy);
		const auto concurrent = delta(current.skippedConcurrent, _renderStatsBaseline.skippedConcurrent);
		const auto latencyMs = delta(current.sourceToDrawMsTotal,
			_renderStatsBaseline.sourceToDrawMsTotal);
		const auto latencySamples = delta(current.sourceToDrawSamples,
			_renderStatsBaseline.sourceToDrawSamples);
		const auto recordUs = delta(current.recordCpuUsTotal,
			_renderStatsBaseline.recordCpuUsTotal);
		const auto recordSamples = delta(current.recordCpuSamples,
			_renderStatsBaseline.recordCpuSamples);

		const RenderStatsSample sample{
			.presentFps = static_cast<double>(presents) / elapsed,
			.drawFps = static_cast<double>(draws) / elapsed,
			.freshFps = static_cast<double>(fresh) / elapsed,
			.submitFps = static_cast<double>(submits) / elapsed,
			.sourceToDrawMs = latencySamples ?
				static_cast<double>(latencyMs) / static_cast<double>(latencySamples) : 0.0,
			.recordCpuMs = recordSamples ?
				static_cast<double>(recordUs) / (1000.0 * static_cast<double>(recordSamples)) : 0.0,
			.reusedDraws = reused,
			.busyWaits = waits,
			.droppedBusy = dropped,
			.skippedConcurrent = concurrent,
			.seamMode = current.seamMode,
			.frameGeneration = current.frameGeneration,
		};
		_renderer->SetRenderStatsSample(sample);
		REX::INFO(
			"Render diagnostics ({:.2f}s): fresh view {:.1f} fps, overlay passes {:.1f}/s "
			"({} reused), frame submit {:.1f} fps, present-hook {:.1f}/s; source-to-draw {:.2f} ms, "
			"record CPU {:.3f} ms; waits {}, dropped {}, concurrent skips {}; path={}, FG={}",
			elapsed, sample.freshFps, sample.drawFps, reused, sample.submitFps, sample.presentFps,
			sample.sourceToDrawMs, sample.recordCpuMs, waits, dropped, concurrent,
			current.seamMode ? "UI seam" : "Present", current.frameGeneration ? "on" : "off");

		_renderStatsBaseline = current;
		_renderStatsLastSampleAt = _uptime;
	}

	std::unique_ptr<IWebRenderer> Runtime::CreateRenderer() const
	{
		if (_config.renderer == "null") {
			return std::make_unique<NullWebRenderer>();
		}
		if (_config.renderer == "mock") {
			return std::make_unique<MockWebRenderer>();
		}
		if (_config.renderer == "webview2") {
#if defined(OSFUI_WITH_WEBVIEW2)
			// Out-of-process host backend: the only WebView2 variant that works
			// under Mod Organizer 2 without the manual executable-blacklist
			// workaround (USVFS injection crashes in-process-spawned browsers).
			// A missing Evergreen runtime is reported by the host over the hello
			// handshake, not probed here — see WebView2HostWebRenderer.
			return std::make_unique<WebView2HostWebRenderer>();
#else
			REX::WARN("Runtime: renderer 'webview2' requested but this build was compiled without "
					  "with_webview2; using null renderer");
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
