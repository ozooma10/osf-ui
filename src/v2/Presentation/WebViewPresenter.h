#pragma once

#include "v2/Runtime/IViewPresenter.h"

#include <chrono>
#include <memory>
#include <unordered_set>

namespace OSFUI
{
    class ICompositor;
    class IWebRenderer;
    struct ViewManifest;
}

namespace Presentation
{
    class WebViewPresenter final : public Runtime::IViewPresenter
    {
    public:
        using DrawAvailable = bool (*)();

        WebViewPresenter(std::unique_ptr<OSFUI::IWebRenderer> a_renderer, std::unique_ptr<OSFUI::ICompositor> a_compositor, DrawAvailable a_drawAvailable);

        ~WebViewPresenter() override;

        bool Initialize(const std::filesystem::path& a_dataDirectory);

        void SetDrawPathInstalled(bool a_installed);

        bool Show(const Runtime::ViewManifest& a_view) noexcept override;

        void Hide(std::string_view a_viewId) noexcept override;
        void Tick() noexcept override;

    private:
        static OSFUI::ViewManifest ConvertManifest(const Runtime::ViewManifest& a_view);

        // Declared first so the renderer is destroyed first. Its worker threads must stop before the compositor it calls into disappears.
        std::unique_ptr<OSFUI::ICompositor> _compositor;
        std::unique_ptr<OSFUI::IWebRenderer> _renderer;

        DrawAvailable _drawAvailable{ nullptr };

        std::unordered_set<std::string> _instantiatedViews;
        std::unordered_set<std::string> _visibleViews;

        std::chrono::steady_clock::time_point _lastTick{};

        bool _initialized{ false };
        bool _drawPathInstalled{ false };
    };
}