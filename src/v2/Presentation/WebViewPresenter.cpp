#include "WebViewPresenter.h"

#include "composite/ICompositor.h"
#include "render/IWebRenderer.h"
#include "runtime/ViewManifest.h"

#include <algorithm>

namespace Presentation
{
    WebViewPresenter::WebViewPresenter(std::unique_ptr<OSFUI::IWebRenderer> a_renderer, std::unique_ptr<OSFUI::ICompositor> a_compositor, DrawAvailable a_drawAvailable) :
        _compositor(std::move(a_compositor)), _renderer(std::move(a_renderer)), _drawAvailable(a_drawAvailable) {}

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
            const OSFUI::RendererConfig rendererConfig{
                .width = OSFUI::kDefaultViewWidth,
                .height = OSFUI::kDefaultViewHeight,
                .devMode = false,
                .dataDir = a_dataDirectory
            };

            if (!_renderer->Initialize(rendererConfig)) {
                REX::ERROR("WebViewPresenter: renderer initialization failed");
                return false;
            }

            if (!_compositor->Initialize()) {
                REX::ERROR("WebViewPresenter: compositor initialization failed");
                return false;
            }

            _renderer->SetSharedRingHandler(
                [this](const OSFUI::SharedRingDesc& a_ring) {
                    _compositor->SetSharedRing(a_ring);
                });

            _compositor->SetOutputResizeCallback(
                [this](std::uint32_t a_width, std::uint32_t a_height) {
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

    bool WebViewPresenter::Show(
        const Runtime::ViewManifest& a_view) noexcept
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
        if (!_initialized) {
            return;
        }

        try {
            _renderer->SetNativeFocus(a_focused);
        } catch (const std::exception& error) {
            REX::ERROR("WebViewPresenter: failed to set input focus to {}: {}", a_focused, error.what());
        } catch (...) {
            REX::ERROR("WebViewPresenter: failed to set input focus to {} with an unknown exception", a_focused);
        }
    }

    void WebViewPresenter::Hide(std::string_view a_viewId) noexcept
    {
        if (!_initialized) {
            return;
        }

        try {
            const std::string viewId{ a_viewId };
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
