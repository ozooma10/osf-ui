#include "runtime/Runtime.h"

#include "composite/D3D12Compositor.h"
#include "composite/NullCompositor.h"
#include "core/Log.h"
#include "core/Paths.h"
#include "render/MockWebRenderer.h"
#include "render/NullWebRenderer.h"
#include "render/UltralightWebRenderer.h"

namespace SWUI
{
	namespace
	{
		// Minimal symbolic-name -> Windows virtual-key mapping for the config
		// toggleKey. Values are VK_* constants spelled numerically so this
		// header-light file does not pull in <Windows.h>.
		KeyCode KeyCodeFromName(std::string_view a_name)
		{
			if (a_name.size() >= 2 && (a_name[0] == 'F' || a_name[0] == 'f')) {
				int n = 0;
				if (std::from_chars(a_name.data() + 1, a_name.data() + a_name.size(), n).ec == std::errc{} &&
					n >= 1 && n <= 24) {
					return 0x70 + static_cast<KeyCode>(n - 1);  // VK_F1..VK_F24
				}
			}
			if (a_name.size() == 1) {
				const auto c = static_cast<unsigned char>(a_name[0]);
				if ((c >= '0' && c <= '9') || (c >= 'A' && c <= 'Z')) {
					return c;  // VK_0..VK_9, VK_A..VK_Z match ASCII
				}
				if (c >= 'a' && c <= 'z') {
					return c - 'a' + 'A';
				}
			}
			REX::WARN("Runtime: unrecognized toggleKey '{}'; toggle key disabled", a_name);
			return 0;
		}
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

		_views.LoadAll(Paths::ViewsDir());

		// Renderer
		_renderer = CreateRenderer();
		const auto* view = _views.Find(_config.view);
		RendererConfig rendererConfig{
			.width = view ? view->width : 1280u,
			.height = view ? view->height : 720u,
			.devMode = _config.devMode,
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
		REX::INFO("Runtime: compositor = {}", _compositor->Name());

		// Active view
		if (view) {
			_renderer->LoadView(*view);
			if (view->permissions.nativeBridge) {
				_bridge = std::make_unique<MessageBridge>(MessageBridge::Host{
					.setVisible = [this](bool a_visible) { SetVisible(a_visible); },
					.sendToWeb = [this](std::string_view a_json) {
						if (_renderer) {
							_renderer->SendMessageToWeb(a_json);
						}
					},
				});
				_bridge->SendRuntimeReady();
			} else {
				REX::INFO("Runtime: view '{}' does not request nativeBridge; bridge disabled", view->id);
			}
		} else {
			REX::WARN("Runtime: configured view '{}' was not found; overlay has no content", _config.view);
		}

		// Input (placeholder: nothing feeds events into the router yet)
		_input.Configure(KeyCodeFromName(_config.toggleKey), [this] { ToggleVisible(); });

		_visible.store(_config.startVisible);
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
		_bridge.reset();
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
			// Stub: Initialize() fails by design and we fall back to null.
			return std::make_unique<D3D12Compositor>();
		}
		if (_config.compositor != "null") {
			REX::WARN("Runtime: unknown compositor '{}'; using null compositor", _config.compositor);
		}
		return std::make_unique<NullCompositor>();
	}
}
