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

    WebViewPresenter::WebViewPresenter(std::unique_ptr<OSFUI::IWebRenderer> a_renderer, std::unique_ptr<OSFUI::ICompositor> a_compositor, DrawAvailable a_drawAvailable, FrameworkKeyHandler a_frameworkKeyHandler)
        : _compositor(std::move(a_compositor)), _renderer(std::move(a_renderer)), _drawAvailable(a_drawAvailable), _frameworkKeyHandler(a_frameworkKeyHandler)
    {}

    WebViewPresenter::~WebViewPresenter() = default;

    bool WebViewPresenter::Initialize(const std::filesystem::path& a_dataDirectory)
    {
        if (_initialized) {
            return true;
        }

        if (!_renderer || !_compositor || !_frameworkKeyHandler) {
            REX::ERROR("WebViewPresenter: renderer, compositor, or framework key handler is unavailable");
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

            _renderer->SetNativeAcceleratorHandler([this](std::uint32_t a_virtualKey, std::uint32_t, bool a_down) { return _frameworkKeyHandler(a_virtualKey, a_down); });
            _renderer->SetWebMessageHandler([this](std::string_view a_viewId, std::string_view a_json) {
                if (_webMessageHandler) {
                    _webMessageHandler(a_viewId, a_json);
                }
            });
            _renderer->SetSharedRingHandler([this](const OSFUI::SharedRingDesc& a_ring) { _compositor->SetSharedRing(a_ring); });
            _renderer->SetLoadHandler([this](const OSFUI::IWebRenderer::LoadEvent& a_event) { HandleLoadResult(a_event.viewId, a_event.failed, a_event.description); });
            _renderer->SetFailureHandler([this](const OSFUI::IWebRenderer::FailureEvent& a_event) {
                std::string detail {a_event.stage};
                if (!a_event.description.empty()) {
                    detail += ": ";
                    detail += a_event.description;
                }
                HandlePresenterFailure(detail);
            });

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

    Runtime::ViewShowResult WebViewPresenter::Show(const Runtime::ViewManifest& a_view) noexcept
    {
        try {
            const bool canDraw = _initialized && !_presenterFailed && _drawPathInstalled && _drawAvailable && _drawAvailable();

            if (!canDraw) {
                REX::WARN("WebViewPresenter: cannot show '{}' because the draw path is unavailable", a_view.id);
                return {};
            }

            bool documentCreated = false;
            if (!_instantiatedViews.contains(a_view.id) || _failedViews.erase(a_view.id) != 0) {
                _loadedDocuments.erase(a_view.id);
                _readyViews.erase(a_view.id);
                _renderer->CreateOrNavigateView(ConvertManifest(a_view));
                _instantiatedViews.insert(a_view.id);
                documentCreated = true;
            }

            _renderer->SetViewHidden(a_view.id, false);

            if (a_view.kind == Runtime::ViewKind::Menu) {
                _renderer->SetInputTargetView(a_view.id);
            }

            _visibleViews.insert(a_view.id);

            if (_readyViews.contains(a_view.id)) {
                _presentationEvents.push_back({.kind = Runtime::ViewPresentationEventKind::Ready, .viewId = a_view.id});
            } else if (_loadedDocuments.contains(a_view.id)) {
                _loadedFrameFloors.insert_or_assign(a_view.id, _lastSubmittedFrameIndex);
            }

            _compositor->SetVisible(HasReadyVisibleView());

            return {.accepted = true, .documentCreated = documentCreated};
        } catch (const std::exception& error) {
            REX::ERROR("WebViewPresenter: failed to show '{}': {}", a_view.id, error.what());
            _visibleViews.erase(a_view.id);
            _readyViews.erase(a_view.id);
            _loadedFrameFloors.erase(a_view.id);
            _failedViews.insert(a_view.id);
            return {};
        } catch (...) {
            REX::ERROR("WebViewPresenter: failed to show '{}' with an unknown exception", a_view.id);
            _visibleViews.erase(a_view.id);
            _readyViews.erase(a_view.id);
            _loadedFrameFloors.erase(a_view.id);
            _failedViews.insert(a_view.id);
            return {};
        }
    }

    void WebViewPresenter::SetWebMessageHandler(WebMessageHandler a_handler)
    {
        _webMessageHandler = std::move(a_handler);
    }

    void WebViewPresenter::SendMessageToWeb(std::string_view a_viewId, std::string_view a_json) noexcept
    {
        if (!_initialized || _presenterFailed) {
            return;
        }

        try {
            _renderer->SendMessageToWeb(a_viewId, a_json);
        } catch (const std::exception& error) {
            REX::ERROR("WebViewPresenter: failed to send a bridge message to '{}': {}", a_viewId, error.what());
            HandlePresenterFailure(error.what());
        } catch (...) {
            REX::ERROR("WebViewPresenter: failed to send a bridge message to '{}' with an unknown exception", a_viewId);
            HandlePresenterFailure("unknown bridge transport failure");
        }
    }

    bool WebViewPresenter::SetInputFocus(bool a_focused) noexcept
    {
        if (!a_focused) {
            _inputFocused.store(false, std::memory_order_release);
            _pendingMousePosition.store(kNoPendingMousePosition, std::memory_order_release);
        }

        if (!_initialized) {
            return false;
        }

        const auto clearNativeInputState = [this]() noexcept {
            _inputFocused.store(false, std::memory_order_release);
            _pendingMousePosition.store(kNoPendingMousePosition, std::memory_order_release);
            try {
                _renderer->SetNativeFocus(false);
            } catch (...) {}
            try {
                _renderer->SetAcceleratorKeys(0, false, false, 0);
            } catch (...) {}
        };

        try {
            if (a_focused) {
                _renderer->SetAcceleratorKeys(0, true, false, 0);
                _renderer->SetNativeFocus(a_focused);
                _inputFocused.store(true, std::memory_order_release);
            } else {
                _renderer->SetNativeFocus(false);
                _renderer->SetAcceleratorKeys(0, false, false, 0);
            }

            return true;
        } catch (const std::exception& error) {
            clearNativeInputState();
            REX::ERROR("WebViewPresenter: failed to set input focus to {}: {}", a_focused, error.what());
        } catch (...) {
            clearNativeInputState();
            REX::ERROR("WebViewPresenter: failed to set input focus to {} with an unknown exception", a_focused);
        }

        return false;
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
            _loadedFrameFloors.erase(viewId);

            _compositor->SetVisible(HasReadyVisibleView());
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

            if (_presenterFailed) {
                hideCompositor();
                return;
            }

            if (const auto frame = _renderer->Render()) {
                _compositor->Submit(*frame);
                PublishReadyViews(frame->frameIndex);
                _lastSubmittedFrameIndex = frame->frameIndex;
            }

            const bool canDraw = _drawPathInstalled && _drawAvailable && _drawAvailable();

            if (!canDraw && !_visibleViews.empty()) {
                HandlePresenterFailure("draw path became unavailable while views were visible");
            }

            _compositor->SetVisible(canDraw && HasReadyVisibleView());
        } catch (const std::exception& error) {
            REX::ERROR("WebViewPresenter: tick failed: {}", error.what());
            hideCompositor();
            HandlePresenterFailure(error.what());
        } catch (...) {
            REX::ERROR("WebViewPresenter: tick failed with an unknown exception");
            hideCompositor();
            HandlePresenterFailure("unknown presenter tick failure");
        }
    }

    std::vector<Runtime::ViewPresentationEvent> WebViewPresenter::TakePresentationEvents()
    {
        std::vector<Runtime::ViewPresentationEvent> events;
        events.swap(_presentationEvents);
        return events;
    }

    void WebViewPresenter::HandleLoadResult(std::string_view a_viewId, bool a_failed, std::string_view a_detail)
    {
        const std::string viewId {a_viewId};

        if (a_failed) {
            _loadedDocuments.erase(viewId);
            _readyViews.erase(viewId);
            _loadedFrameFloors.erase(viewId);
            _failedViews.insert(viewId);
            if (_visibleViews.contains(viewId)) {
                _presentationEvents.push_back({.kind = Runtime::ViewPresentationEventKind::ViewFailed, .viewId = viewId, .detail = std::string {a_detail}});
            }
            return;
        }

        _loadedDocuments.insert(viewId);
        _readyViews.erase(viewId);
        if (_visibleViews.contains(viewId)) {
            _presentationEvents.push_back({.kind = Runtime::ViewPresentationEventKind::NotReady, .viewId = viewId});
            _loadedFrameFloors.insert_or_assign(viewId, _lastSubmittedFrameIndex);
        }
    }

    void WebViewPresenter::HandlePresenterFailure(std::string_view a_detail) noexcept
    {
        if (_presenterFailed) {
            return;
        }

        _presenterFailed = true;
        _inputFocused.store(false, std::memory_order_release);
        _pendingMousePosition.store(kNoPendingMousePosition, std::memory_order_release);
        _loadedDocuments.clear();
        _readyViews.clear();
        _loadedFrameFloors.clear();

        try {
            _presentationEvents.push_back({.kind = Runtime::ViewPresentationEventKind::PresenterFailed, .detail = std::string {a_detail}});
        } catch (...) {
            // The coordinator cannot recover useful presentation state after a terminal backend failure. 
            // Keep the compositor and input locally off even if allocating the diagnostic event itself failed.
        }
    }

    void WebViewPresenter::PublishReadyViews(std::uint64_t a_frameIndex)
    {
        for (auto iterator = _loadedFrameFloors.begin(); iterator != _loadedFrameFloors.end();) {
            const auto& [viewId, frameFloor] = *iterator;
            const bool newerFrame = !frameFloor || a_frameIndex > *frameFloor;

            if (!_visibleViews.contains(viewId)) {
                iterator = _loadedFrameFloors.erase(iterator);
                continue;
            }

            if (!newerFrame) {
                ++iterator;
                continue;
            }

            _readyViews.insert(viewId);
            _failedViews.erase(viewId);
            _presentationEvents.push_back({.kind = Runtime::ViewPresentationEventKind::Ready, .viewId = viewId});
            iterator = _loadedFrameFloors.erase(iterator);
        }
    }

    bool WebViewPresenter::HasReadyVisibleView() const
    {
        return std::ranges::any_of(_visibleViews, [this](const auto& a_viewId) { return _readyViews.contains(a_viewId); });
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
        converted.permissions = {.nativeBridge = true, .filesystem = false, .network = false};
        converted.rootDir = a_view.rootDirectory;

        return converted;
    }
}
