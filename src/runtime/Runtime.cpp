#include "runtime/Runtime.h"

#include <cmath>

#include "composite/D3D12Compositor.h"
#include "composite/NullCompositor.h"
#include "core/Log.h"
#include "core/Paths.h"
#include "platform/WindowsPlatform.h"
#include "render/MockWebRenderer.h"
#include "runtime/Json.h"
#include "render/NullWebRenderer.h"
#include "render/UltralightWebRenderer.h"

namespace SWUI
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

		// Active view. Bridge and web->native handler are wired BEFORE
		// LoadView so no early page message can race past them; renderers
		// queue native->web messages until the page is actually ready.
		if (view) {
			if (view->permissions.nativeBridge) {
				_bridge = std::make_unique<MessageBridge>([this](std::string_view a_json) {
					if (_renderer) {
						_renderer->SendMessageToWeb(a_json);
					}
				});
				// Platform (window) commands live in core; everything else is a
				// module's to register.
				RegisterPlatformCommands(*_bridge);
				for (const auto& module : _modules) {
					module->RegisterCommands(*_bridge);
				}
				_renderer->SetWebMessageHandler([this](std::string_view a_json) {
					if (_bridge) {
						_bridge->HandleWebMessage(a_json);
					}
				});
			} else {
				REX::INFO("Runtime: view '{}' does not request nativeBridge; bridge disabled", view->id);
			}
			_renderer->LoadView(*view);
			if (_bridge) {
				_bridge->SendRuntimeReady();
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
		if (!_initialized || !_renderer) {
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

	bool Runtime::IsInputCaptured() const
	{
		return _initialized && _captureInput.load() && _visible.load();
	}

	bool Runtime::OnHostKey(std::uint32_t a_vkCode, bool a_down)
	{
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
			: docs / "My Games" / "Starfield" / "StarfieldWebUI" / "settings";
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
		a_bridge.RegisterCommand("log", [](const nlohmann::json& a_p, MessageBridge&) {
			// Untrusted content: bound the length so JS cannot flood the log.
			REX::INFO("MessageBridge: [web] {}", Json::GetString(a_p, "text", "").substr(0, 512));
		});
		a_bridge.RegisterCommand("ping", [](const nlohmann::json&, MessageBridge& a_b) {
			a_b.SendToWeb("runtime.pong", nlohmann::json::object());
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
#if defined(SWUI_WITH_ULTRALIGHT)
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
			// Phase 2: uploads frames to a GPU texture on the game's device
			// (located lazily; see composite/EngineD3D12.h). No drawing yet.
			return std::make_unique<D3D12Compositor>();
		}
		if (_config.compositor != "null") {
			REX::WARN("Runtime: unknown compositor '{}'; using null compositor", _config.compositor);
		}
		return std::make_unique<NullCompositor>();
	}
}
