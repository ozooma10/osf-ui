#pragma once

#include "v2/Bridge/IViewMessageTransport.h"
#include "v2/Runtime/IViewPresenter.h"

#include <atomic>
#include <chrono>
#include <cstdint>
#include <memory>
#include <optional>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace OSFUI
{
    class ICompositor;
    class IWebRenderer;
    struct ViewManifest;
}

namespace Presentation
{
    class WebViewPresenter final : public Runtime::IViewPresenter, public Bridge::IViewMessageTransport
    {
    public:
        using DrawAvailable = bool (*)();
        using FrameworkKeyHandler = bool (*)(std::uint32_t, bool);

        WebViewPresenter(std::unique_ptr<OSFUI::IWebRenderer> a_renderer, std::unique_ptr<OSFUI::ICompositor> a_compositor, DrawAvailable a_drawAvailable, FrameworkKeyHandler a_frameworkKeyHandler);

        ~WebViewPresenter() override;

        bool Initialize(const std::filesystem::path& a_dataDirectory);

        void SetDrawPathInstalled(bool a_installed);

        Runtime::ViewShowResult Show(const Runtime::ViewManifest& a_view) noexcept override;

        void SetWebMessageHandler(WebMessageHandler a_handler) override;
        void SendMessageToWeb(std::string_view a_viewId, std::string_view a_json) noexcept override;

        bool SetInputFocus(bool a_focused) noexcept override;
        void SendKeyEvent(std::uint32_t a_virtualKey, bool a_down) noexcept override;
        void UpdateMousePosition(int a_clientX, int a_clientY, int a_clientWidth, int a_clientHeight) noexcept override;
        void SendMouseButtonEvent(int a_button, bool a_down) noexcept override;
        void SendMouseWheelEvent(int a_wheelDelta) noexcept override;

        void Hide(std::string_view a_viewId) noexcept override;
        void Tick() noexcept override;
        std::vector<Runtime::ViewPresentationEvent> TakePresentationEvents() override;

    private:
        static OSFUI::ViewManifest ConvertManifest(const Runtime::ViewManifest& a_view);

        void HandleLoadResult(std::string_view a_viewId, bool a_failed, std::string_view a_detail);
        void HandlePresenterFailure(std::string_view a_detail) noexcept;
        void PublishReadyViews(std::uint64_t a_frameIndex);
        bool HasReadyVisibleView() const;

        static constexpr std::uint64_t kNoPendingMousePosition = ~std::uint64_t {0};

        // Declared first so the renderer is destroyed first. Its worker threads must stop before the compositor it calls into disappears.
        std::unique_ptr<OSFUI::ICompositor> _compositor;
        std::unique_ptr<OSFUI::IWebRenderer> _renderer;

        DrawAvailable _drawAvailable {nullptr};
        FrameworkKeyHandler _frameworkKeyHandler {nullptr};

        std::unordered_set<std::string> _instantiatedViews;
        std::unordered_set<std::string> _visibleViews;
        std::unordered_set<std::string> _loadedDocuments;
        std::unordered_set<std::string> _readyViews;
        std::unordered_set<std::string> _failedViews;
        std::unordered_map<std::string, std::optional<std::uint64_t>> _loadedFrameFloors;
        std::vector<Runtime::ViewPresentationEvent> _presentationEvents;
        WebMessageHandler _webMessageHandler;

        std::chrono::steady_clock::time_point _lastTick {};
        std::optional<std::uint64_t> _lastSubmittedFrameIndex;

        // Keep coordinate pairs coherent while input and presentation run on different threads.
        std::atomic<std::uint64_t> _outputSize {0};
        std::atomic<std::uint64_t> _mousePosition {0};
        std::atomic<std::uint64_t> _pendingMousePosition {kNoPendingMousePosition};
        std::atomic_bool _inputFocused {false};

        bool _initialized {false};
        bool _drawPathInstalled {false};
        bool _presenterFailed {false};
    };
}
