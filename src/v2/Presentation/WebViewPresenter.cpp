#include "WebViewPresenter.h"

#include "composite/ICompositor.h"
#include "render/IWebRenderer.h"
#include "runtime/ViewManifest.h"

#include <algorithm>

namespace Presentation
{
    namespace
    {
        std::uint64_t PackPair(std::uint32_t a_first, std::uint32_t a_second)
        {
            return (static_cast<std::uint64_t>(a_first) << 32) | a_second;
        }

        std::uint32_t UnpackFirst(std::uint64_t a_pair)
        {
            return static_cast<std::uint32_t>(a_pair >> 32);
        }

        std::uint32_t UnpackSecond(std::uint64_t a_pair)
        {
            return static_cast<std::uint32_t>(a_pair);
        }

        std::uint32_t ScaleCoordinate(int a_clientCoordinate, int a_clientExtent, std::uint32_t a_outputExtent)
        {
            const auto scaled = static_cast<std::int64_t>(a_clientCoordinate) * a_outputExtent / a_clientExtent;
            return static_cast<std::uint32_t>(std::clamp(scaled, std::int64_t {0}, static_cast<std::int64_t>(a_outputExtent - 1)));
        }
    }

    WebViewPresenter::WebViewPresenter(std::unique_ptr<OSFUI::IWebRenderer> a_renderer, std::unique_ptr<OSFUI::ICompositor> a_compositor, DrawAvailable a_drawAvailable)
        : _compositor(std::move(a_compositor)), _renderer(std::move(a_renderer)), _drawAvailable(a_drawAvailable)
    {}

    WebViewPresenter::~WebViewPresenter() = default;

    bool WebViewPresenter::Initialize(const std::filesystem::path& a_dataDirectory)
    {
        if (_initialized) {
            return true;
        }

        if (!_renderer || !_compositor) {
            REX::ERROR("WebViewPresenter: renderer or compositor is unavailable");
            return false;
        }

        try {
            const OSFUI::RendererConfig rendererConfig {.width = OSFUI::kDefaultViewWidth, .height = OSFUI::kDefaultViewHeight, .devMode = false, .dataDir = a_dataDirectory};

            _outputSize.store(PackPair(rendererConfig.width, rendererConfig.height), std::memory_order_release);

            if (!_renderer->Initialize(rendererConfig)) {
                REX::ERROR("WebViewPresenter: renderer initialization failed");
                return false;
            }

            if (!_compositor->Initialize()) {
                REX::ERROR("WebViewPresenter: compositor initialization failed");
                return false;
            }

            _renderer->SetSharedRingHandler([this](const OSFUI::SharedRingDesc& a_ring) { _compositor->SetSharedRing(a_ring); });

            _compositor->SetOutputResizeCallback([this](std::uint32_t a_width, std::uint32_t a_height) {
                _outputSize.store(PackPair(a_width, a_height), std::memory_order_release);
                _renderer->Resize(a_width, a_height);
            });

            _compositor->SetVisible(false);

            _lastTick = std::chrono::steady_clock::now();
            _initialized = true;

            REX::INFO("WebViewPresenter: initialized renderer '{}' and compositor '{}'", _renderer->Name(), _compositor->Name());

            return true;
        } catch (const std::exception& error) {
            REX::ERROR("WebViewPresenter: initialization failed: {}", error.what());
            return false;
        } catch (...) {
            REX::ERROR("WebViewPresenter: initialization failed with an unknown exception");
            return false;
        }
    }

    void WebViewPresenter::SetDrawPathInstalled(bool a_installed)
    {
        _drawPathInstalled = a_installed;

        if (!_initialized) {
            return;
        }

        _compositor->SetScaleformOverlayEnabled(a_installed);

        if (!a_installed) {
            _compositor->SetVisible(false);
        }
    }

    bool WebViewPresenter::Show(const Runtime::ViewManifest& a_view) noexcept
    {
        try {
            const bool canDraw = _initialized && _drawPathInstalled && _drawAvailable && _drawAvailable();

            if (!canDraw) {
                REX::WARN("WebViewPresenter: cannot show '{}' because the draw path is unavailable", a_view.id);
                return false;
            }

            if (!_instantiatedViews.contains(a_view.id)) {
                _renderer->CreateOrNavigateView(ConvertManifest(a_view));
                _instantiatedViews.insert(a_view.id);
            }

            _renderer->SetViewHidden(a_view.id, false);

            if (a_view.kind == Runtime::ViewKind::Menu) {
                _renderer->SetInputTargetView(a_view.id);
            }

            _visibleViews.insert(a_view.id);
            _compositor->SetVisible(true);

            return true;
        } catch (const std::exception& error) {
            REX::ERROR("WebViewPresenter: failed to show '{}': {}", a_view.id, error.what());
            return false;
        } catch (...) {
            REX::ERROR("WebViewPresenter: failed to show '{}' with an unknown exception", a_view.id);
            return false;
        }
    }

    void WebViewPresenter::SetInputFocus(bool a_focused) noexcept
    {
        if (!a_focused) {
            _inputFocused.store(false, std::memory_order_release);
            _pendingMousePosition.store(kNoPendingMousePosition, std::memory_order_release);
        }

        if (!_initialized) {
            return;
        }

        try {
            _renderer->SetNativeFocus(a_focused);

            if (a_focused) {
                _inputFocused.store(true, std::memory_order_release);
            }
        } catch (const std::exception& error) {
            REX::ERROR("WebViewPresenter: failed to set input focus to {}: {}", a_focused, error.what());
        } catch (...) {
            REX::ERROR("WebViewPresenter: failed to set input focus to {} with an unknown exception", a_focused);
        }
    }

    void WebViewPresenter::SendKeyEvent(std::uint32_t a_virtualKey, bool a_down) noexcept
    {
        if (!_inputFocused.load(std::memory_order_acquire)) {
            return;
        }

        try {
            _renderer->InjectKeyEvent(a_virtualKey, a_down);
        } catch (const std::exception& error) {
            REX::ERROR("WebViewPresenter: failed to send key event: {}", error.what());
        } catch (...) {
            REX::ERROR("WebViewPresenter: failed to send key event with an unknown exception");
        }
    }

    void WebViewPresenter::UpdateMousePosition(int a_clientX, int a_clientY, int a_clientWidth, int a_clientHeight) noexcept
    {
        if (!_inputFocused.load(std::memory_order_acquire) || a_clientWidth <= 0 || a_clientHeight <= 0) {
            return;
        }

        const auto outputSize = _outputSize.load(std::memory_order_acquire);
        const auto outputWidth = UnpackFirst(outputSize);
        const auto outputHeight = UnpackSecond(outputSize);

        if (outputWidth == 0 || outputHeight == 0) {
            return;
        }

        const auto position = PackPair(ScaleCoordinate(a_clientX, a_clientWidth, outputWidth), ScaleCoordinate(a_clientY, a_clientHeight, outputHeight));

        _mousePosition.store(position, std::memory_order_release);
        _pendingMousePosition.store(position, std::memory_order_release);
    }

    void WebViewPresenter::SendMouseButtonEvent(int a_button, bool a_down) noexcept
    {
        if (!_inputFocused.load(std::memory_order_acquire)) {
            return;
        }

        const auto position = _mousePosition.load(std::memory_order_acquire);

        try {
            _renderer->InjectMouseButton(static_cast<int>(UnpackFirst(position)), static_cast<int>(UnpackSecond(position)), a_button, a_down);
        } catch (const std::exception& error) {
            REX::ERROR("WebViewPresenter: failed to send mouse button event: {}", error.what());
        } catch (...) {
            REX::ERROR("WebViewPresenter: failed to send mouse button event with an unknown exception");
        }
    }

    void WebViewPresenter::SendMouseWheelEvent(int a_wheelDelta) noexcept
    {
        if (!_inputFocused.load(std::memory_order_acquire)) {
            return;
        }

        const auto position = _mousePosition.load(std::memory_order_acquire);

        try {
            _renderer->InjectPhysicalMouseWheel(static_cast<int>(UnpackFirst(position)), static_cast<int>(UnpackSecond(position)), a_wheelDelta);
        } catch (const std::exception& error) {
            REX::ERROR("WebViewPresenter: failed to send mouse wheel event: {}", error.what());
        } catch (...) {
            REX::ERROR("WebViewPresenter: failed to send mouse wheel event with an unknown exception");
        }
    }

    void WebViewPresenter::Hide(std::string_view a_viewId) noexcept
    {
        if (!_initialized) {
            return;
        }

        try {
            const std::string viewId {a_viewId};
            if (!_instantiatedViews.contains(viewId)) {
                return;
            }

            _renderer->SetViewHidden(viewId, true);
            _visibleViews.erase(viewId);

            if (_visibleViews.empty()) {
                _compositor->SetVisible(false);
            }
        } catch (const std::exception& error) {
            REX::ERROR("WebViewPresenter: failed to hide '{}': {}", a_viewId, error.what());
        } catch (...) {
            REX::ERROR("WebViewPresenter: failed to hide '{}' with an unknown exception", a_viewId);
        }
    }

    void WebViewPresenter::Tick() noexcept
    {
        if (!_initialized) {
            return;
        }

        const auto hideCompositor = [this]() noexcept {
            try {
                _compositor->SetVisible(false);
            } catch (...) {
                // Tick is the outer runtime safety boundary. A broken backend must
                // not escape while we are already containing another failure.
            }
        };

        try {
            const auto now = std::chrono::steady_clock::now();
            const auto elapsed = std::chrono::duration<double>(now - _lastTick).count();
            _lastTick = now;

            const double deltaSeconds = std::clamp(elapsed, 0.0, 0.1);

            const auto mousePosition = _pendingMousePosition.exchange(kNoPendingMousePosition, std::memory_order_acq_rel);

            if (mousePosition != kNoPendingMousePosition && _inputFocused.load(std::memory_order_acquire)) {
                _renderer->InjectMouseMove(static_cast<int>(UnpackFirst(mousePosition)), static_cast<int>(UnpackSecond(mousePosition)));
            }

            _renderer->Update(deltaSeconds);

            if (const auto frame = _renderer->Render()) {
                _compositor->Submit(*frame);
            }

            const bool canDraw = _drawPathInstalled && _drawAvailable && _drawAvailable();

            _compositor->SetVisible(canDraw && !_visibleViews.empty());
        } catch (const std::exception& error) {
            REX::ERROR("WebViewPresenter: tick failed: {}", error.what());
            hideCompositor();
        } catch (...) {
            REX::ERROR("WebViewPresenter: tick failed with an unknown exception");
            hideCompositor();
        }
    }

    OSFUI::ViewManifest WebViewPresenter::ConvertManifest(const Runtime::ViewManifest& a_view)
    {
        OSFUI::ViewManifest converted;

        converted.id = a_view.id;
        converted.title = a_view.title;
        converted.entry = a_view.entry;
        converted.width = a_view.width;
        converted.height = a_view.height;
        converted.transparent = a_view.transparent;

        converted.kind = a_view.kind == Runtime::ViewKind::Hud ? OSFUI::ViewKind::Hud : OSFUI::ViewKind::Menu;

        converted.menuInputEligible = converted.kind == OSFUI::ViewKind::Menu;

        converted.capturesInput = a_view.capturesInput;
        converted.pausesGame = a_view.pausesGame;
        converted.openOnStart = a_view.openOnStart;
        converted.rootDir = a_view.rootDirectory;

        return converted;
    }
}
