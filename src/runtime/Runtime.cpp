#include "runtime/Runtime.h"

#include <cmath>

#include "RE/C/Calendar.h"

#include "composite/D3D12Compositor.h"
#include "composite/NullCompositor.h"
#include "core/Log.h"
#include "input/ControlLayer.h"
#include "input/FocusMenu.h"
#include "core/Paths.h"
#include "platform/WindowsPlatform.h"
#include "render/MockWebRenderer.h"
#include "runtime/Json.h"
#include "render/NullWebRenderer.h"
#include "render/UltralightWebRenderer.h"

namespace PrismaSF
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

		// Route per-view DOM-ready to the consumer API's CreateView callback
		// (no-op for config/declarative views). Delivered on the game thread.
		_renderer->SetDomReadyHandler([this](std::string_view a_viewId) {
			if (auto cb = _apiViews.DomReadyFor(a_viewId)) {
				cb();
			}
		});

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

			// Load the layer set (layering = manifest zorder; focus = config.view).
			std::size_t loaded = 0;
			for (const auto& id : toLoad) {
				if (const auto* m = _views.Find(id)) {
					_renderer->LoadView(*m);
					++loaded;
				} else {
					REX::WARN("Runtime: configured view '{}' not found; skipping", id);
				}
			}
			_renderer->SetActiveView(_config.view);
			REX::INFO("Runtime: loaded {} view(s); active = '{}'", loaded, _config.view);

			// Focusable (interactive) views, in load order, and the index of the
			// one that starts active. The focusKey cycles through these.
			for (const auto& id : toLoad) {
				if (const auto* m = _views.Find(id); m && m->interactive) {
					_interactiveViews.push_back(id);
				}
			}
			for (std::size_t i = 0; i < _interactiveViews.size(); ++i) {
				if (_interactiveViews[i] == _config.view) {
					_activeViewIndex = i;
					break;
				}
			}

			// Greet each bridge-enabled view. The renderer queues this per view
			// until that view's DOM is ready, so order here doesn't matter.
			if (_bridge) {
				for (const auto& id : bridgeViews) {
					_bridge->SendRuntimeReady(id);
				}
			}
		} else {
			REX::WARN("Runtime: configured view '{}' was not found; overlay has no content", _config.view);
		}

		// Input. Events reach the router only when the UiInputHook is
		// installed and enabled (config inputSource="ui", wired in
		// core/Plugin.cpp at kPostPostDataLoad).
		_toggleKey = ResolveKeyName(_config.toggleKey);
		if (_toggleKey != kInvalidKeyCode) {
			REX::INFO("Runtime: toggleKey '{}' resolved to VK code {:#x}", _config.toggleKey, _toggleKey);
		}
		_input.Configure(_toggleKey, [this] { ToggleVisible(); });

		_focusKey = ResolveKeyName(_config.focusKey);
		if (_focusKey != kInvalidKeyCode) {
			REX::INFO("Runtime: focusKey '{}' resolved to VK code {:#x} ({} interactive view(s))",
				_config.focusKey, _focusKey, _interactiveViews.size());
		}

		// Keyboard routing into the web view, gated by capture state (Phase 4).
		_input.SetWebRouting(
			[this] { return IsInputCaptured(); },
			[this](KeyCode a_key, bool a_down) {
				if (_renderer) {
					_renderer->InjectKeyEvent(a_key, a_down);
				}
			});
		REX::INFO("Runtime: input capture {} (config captureInput)", _config.captureInput ? "enabled" : "disabled");

		_visible.store(_config.startVisible);
		if (_compositor) {
			_compositor->SetVisible(_config.startVisible);
		}
		REX::INFO("Runtime: initialized (visible={})", _visible.load());

		_initialized = true;
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
		// Open/close the engine focus menu on the MAIN thread (visibility is
		// flipped from the WndProc/input thread, but UIMessageQueue must only be
		// touched here). No-op unless config.focusMenu is set. EXPERIMENTAL.
		if (_config.focusMenu) {
			ReconcileFocusMenu();
		}
		if (_config.disableControls) {
			ReconcileControlLayer();
		}
		if (!_renderer) {
			return;
		}
		_renderer->Update(a_deltaSeconds);
		SubmitFrameIfVisible();
	}

	void Runtime::SetVisible(bool a_visible)
	{
		const bool was = _visible.exchange(a_visible);
		if (was != a_visible) {
			REX::INFO("Runtime: overlay visibility -> {}", a_visible);
			if (_compositor) {
				_compositor->SetVisible(a_visible);
			}
			// Re-center the virtual cursor each time the overlay opens so it
			// always starts in a known, on-screen spot.
			if (a_visible) {
				_cursorX = _viewWidth.load() * 0.5f;
				_cursorY = _viewHeight.load() * 0.5f;
				if (_renderer) {
					_renderer->InjectMouseMove(static_cast<int>(_cursorX), static_cast<int>(_cursorY));
				}
			}
		}
	}

	void Runtime::ToggleVisible()
	{
		SetVisible(!_visible.load());
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

	bool Runtime::IsInputCaptured() const
	{
		return _initialized && _captureInput.load() && _visible.load();
	}

	bool Runtime::OnHostKey(std::uint32_t a_vkCode, bool a_down)
	{
		// Focus switch (focusKey): cycle the active interactive view. Only while
		// captured AND with more than one interactive view — otherwise the key
		// passes through normally (e.g. Tab still navigates fields when there is a
		// single interactive view). Consume both transitions so the key reaches
		// neither a view nor the game.
		if (a_vkCode == _focusKey && _focusKey != kInvalidKeyCode &&
			IsInputCaptured() && _interactiveViews.size() > 1) {
			if (a_down) {
				CycleActiveView();
			}
			return true;
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

	void Runtime::OnHostMouseDelta(int a_dx, int a_dy)
	{
		if (!IsInputCaptured() || !_renderer) {
			return;
		}
		// Scale raw deltas so the cursor crosses the view in a screen-size-
		// independent amount of physical mouse travel (the view tracks the
		// screen now, so a fixed 1:1 mapping would feel slow on big views),
		// times the user's live cursor-speed setting (prismasf.cursorSpeed).
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
		// pixels via the per-view scroll step (consumer API SetScrollingPixelSize).
		_renderer->InjectMouseWheel(static_cast<int>(_cursorX), static_cast<int>(_cursorY), a_wheelDelta);
	}

	void Runtime::CycleActiveView()
	{
		if (_interactiveViews.size() < 2 || !_renderer) {
			return;
		}
		// Move the OLD active view's software cursor off-screen (InjectMouseMove
		// targets the current active view), switch focus, then place the cursor in
		// the NEW active view at the same spot — so only one cursor is ever shown.
		_renderer->InjectMouseMove(-1000, -1000);
		_activeViewIndex = (_activeViewIndex + 1) % _interactiveViews.size();
		const auto& id = _interactiveViews[_activeViewIndex];
		_renderer->SetActiveView(id);
		_renderer->InjectMouseMove(static_cast<int>(_cursorX), static_cast<int>(_cursorY));
		REX::INFO("Runtime: focus -> view '{}'", id);
	}

	void Runtime::ReconcileFocusMenu()
	{
		// Runs on the game main thread (Tick). Drive the engine menu's open state
		// toward the overlay's visibility; only act on a change so we don't spam
		// the UI message queue every frame.
		const bool wantOpen = _visible.load() && !_apiSuppressFocusMenu.load();
		if (wantOpen == _focusMenuOpen) {
			return;
		}
		_focusMenuOpen = wantOpen;
		if (wantOpen) {
			FocusMenu::Open();
		} else {
			FocusMenu::Close();
		}
	}

	void Runtime::ReconcileControlLayer()
	{
		// Main-thread (Tick). Drive the input-enable layer toward overlay
		// visibility. Engage() may no-op until gameplay (manager not ready at the
		// main menu); IsEngaged() stays false then, so we simply retry next tick.
		const bool wantEngaged = _visible.load();
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
			: docs / "My Games" / "Starfield" / "PrismaUI" / "settings";
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
		a_bridge.RegisterCommand("close", [this](const nlohmann::json&, MessageBridge&) {
			SetVisible(false);
		});
		a_bridge.RegisterCommand("setVisible", [this](const nlohmann::json& a_p, MessageBridge&) {
			SetVisible(Json::GetBool(a_p, "visible", false));
		});
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
		if (a_modId == "prismasf" && a_key == "cursorSpeed" && a_value.is_number()) {
			const auto speed = a_value.get<float>();
			_cursorSpeed.store(speed);
			REX::INFO("Runtime: setting prismasf.cursorSpeed -> {:.2f}", speed);
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
#if defined(PRISMA_SF_WITH_ULTRALIGHT)
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

	// ===================== Public consumer API (src/api) =====================
	// These back the exported RequestPluginAPI vtable (PRISMA_UI_API). The handle
	// table (_apiViews) is the source of truth for logical per-view state; the
	// renderer is told to honor it. See Runtime.h for the threading contract.

	std::uint64_t Runtime::ApiCreateView(std::string a_htmlPath, ViewRegistry::DomReadyCb a_onDomReady)
	{
		if (!_initialized || !_renderer || a_htmlPath.empty()) {
			REX::WARN("Runtime: ApiCreateView('{}') ignored (runtime/renderer not ready)", a_htmlPath);
			return 0;
		}

		const auto handle = _apiViews.Create(a_htmlPath, std::move(a_onDomReady));
		const auto id = _apiViews.InternalId(handle);

		// htmlPath is "<folder>/<entry>" relative to the views dir (PrismaUI
		// convention, e.g. "MyMod/index.html"). The renderer builds the file URL
		// from the manifest's rootDir folder + entry and keys the view by id.
		std::string folder = a_htmlPath;
		std::string entry = "index.html";
		if (const auto slash = a_htmlPath.find('/'); slash != std::string::npos) {
			folder = a_htmlPath.substr(0, slash);
			entry = a_htmlPath.substr(slash + 1);
		} else {
			REX::WARN("Runtime: ApiCreateView htmlPath '{}' has no '<folder>/' prefix; "
					  "expected e.g. \"MyMod/index.html\"", a_htmlPath);
		}

		ViewManifest m;
		m.id = id;
		m.title = a_htmlPath;
		m.entry = entry;
		m.transparent = true;
		m.zorder = _apiViews.GetOrder(handle);
		m.interactive = true;
		m.permissions.nativeBridge = true;  // trusted native consumer: full bridge + Invoke
		m.rootDir = Paths::ViewsDir() / folder;

		_renderer->LoadView(m);
		REX::INFO("Runtime: ApiCreateView '{}' -> handle {} (view '{}')", a_htmlPath, handle, id);
		return handle;
	}

	void Runtime::ApiDestroy(std::uint64_t a_handle)
	{
		const auto id = _apiViews.Remove(a_handle);
		if (!id.empty() && _renderer) {
			_renderer->DestroyView(id);
			REX::INFO("Runtime: ApiDestroy view '{}'", id);
		}
	}

	bool Runtime::ApiIsValid(std::uint64_t a_handle) const
	{
		return _apiViews.Exists(a_handle);
	}

	void Runtime::ApiInvoke(std::uint64_t a_handle, std::string a_script, IWebRenderer::ScriptResultHandler a_onResult)
	{
		const auto id = _apiViews.InternalId(a_handle);
		if (id.empty() || !_renderer) {
			return;
		}
		_renderer->EvaluateScript(id, a_script, std::move(a_onResult));
	}

	void Runtime::ApiInteropCall(std::uint64_t a_handle, std::string a_fn, std::string a_arg)
	{
		const auto id = _apiViews.InternalId(a_handle);
		if (id.empty() || !_renderer) {
			return;
		}
		_renderer->CallJsFunction(id, a_fn, a_arg);
	}

	void Runtime::ApiRegisterJSListener(std::uint64_t a_handle, std::string a_name, IWebRenderer::JsListenerHandler a_cb)
	{
		const auto id = _apiViews.InternalId(a_handle);
		if (id.empty() || !_renderer) {
			return;
		}
		_renderer->RegisterJsFunction(id, a_name, std::move(a_cb));
	}

	void Runtime::ApiRegisterConsoleCallback(std::uint64_t a_handle, ViewRegistry::ConsoleCb a_cb)
	{
		const auto id = _apiViews.InternalId(a_handle);
		if (id.empty() || !_renderer) {
			return;
		}
		const bool has = static_cast<bool>(a_cb);
		_apiViews.SetConsole(a_handle, std::move(a_cb));
		if (has) {
			_renderer->SetConsoleHandler(id, [this, id](int a_level, std::string a_msg) {
				if (auto cb = _apiViews.ConsoleFor(id)) {
					cb(a_level, std::move(a_msg));
				}
			});
		} else {
			_renderer->SetConsoleHandler(id, nullptr);
		}
	}

	bool Runtime::ApiHasFocus(std::uint64_t a_handle) const
	{
		return IsInputCaptured() && _apiViews.IsFocused(a_handle);
	}

	bool Runtime::ApiFocus(std::uint64_t a_handle, bool a_pauseGame, bool a_disableFocusMenu)
	{
		const auto id = _apiViews.InternalId(a_handle);
		if (id.empty() || !_renderer) {
			return false;
		}
		_apiViews.SetFocused(a_handle, true, /*exclusive*/ true);
		_apiViews.SetHidden(a_handle, false);
		_apiSuppressFocusMenu.store(a_disableFocusMenu);
		_captureInput.store(true);  // a focused view owns input
		_renderer->SetViewHidden(id, false);
		_renderer->SetActiveView(id);
		SetVisible(true);  // overlay must be up to render/capture
		if (a_pauseGame) {
			ControlLayer::Engage();  // EXPERIMENTAL device-agnostic freeze (incl. gamepad)
		}
		REX::INFO("Runtime: ApiFocus view '{}' (pauseGame={}, disableFocusMenu={})",
			id, a_pauseGame, a_disableFocusMenu);
		return true;
	}

	void Runtime::ApiUnfocus(std::uint64_t a_handle)
	{
		_apiViews.SetFocused(a_handle, false, /*exclusive*/ false);
		_apiSuppressFocusMenu.store(false);
		ControlLayer::Release();  // no-op if not engaged
	}

	bool Runtime::ApiHasAnyActiveFocus() const
	{
		return IsInputCaptured() && _apiViews.AnyFocused();
	}

	void Runtime::ApiShow(std::uint64_t a_handle)
	{
		if (!_apiViews.Exists(a_handle)) {
			return;
		}
		_apiViews.SetHidden(a_handle, false);
		if (_renderer) {
			_renderer->SetViewHidden(_apiViews.InternalId(a_handle), false);
		}
		SetVisible(true);  // ensure the overlay is up so the shown view renders
	}

	void Runtime::ApiHide(std::uint64_t a_handle)
	{
		if (!_apiViews.Exists(a_handle)) {
			return;
		}
		_apiViews.SetHidden(a_handle, true);
		if (_renderer) {
			_renderer->SetViewHidden(_apiViews.InternalId(a_handle), true);
		}
		// Leave the overlay visible — other views may still be shown.
	}

	bool Runtime::ApiIsHidden(std::uint64_t a_handle) const
	{
		return _apiViews.IsHidden(a_handle);
	}

	void Runtime::ApiSetOrder(std::uint64_t a_handle, int a_order)
	{
		_apiViews.SetOrder(a_handle, a_order);
		if (_renderer) {
			_renderer->SetViewOrder(_apiViews.InternalId(a_handle), a_order);
		}
	}

	int Runtime::ApiGetOrder(std::uint64_t a_handle) const
	{
		return _apiViews.GetOrder(a_handle);
	}

	void Runtime::ApiSetScrollingPixelSize(std::uint64_t a_handle, int a_px)
	{
		_apiViews.SetScrollPixelSize(a_handle, a_px);
		if (_renderer) {
			_renderer->SetScrollPixelSize(_apiViews.InternalId(a_handle), a_px);
		}
	}

	int Runtime::ApiGetScrollingPixelSize(std::uint64_t a_handle) const
	{
		return _apiViews.GetScrollPixelSize(a_handle);
	}
}
