#pragma once

#include "ViewManifest.h"

#include <cstdint>

namespace Runtime
{
    class IViewPresenter
    {
    public:
        virtual ~IViewPresenter() = default;

        virtual bool Show(const ViewManifest& a_view) noexcept = 0;

        virtual void SetInputFocus(bool a_focused) noexcept = 0;
        // Game-window input can arrive outside the main thread. Implementations must synchronize any shared state they touch.
        virtual void SendKeyEvent(std::uint32_t a_virtualKey, bool a_down) noexcept = 0;
        virtual void UpdateMousePosition(int a_clientX, int a_clientY, int a_clientWidth, int a_clientHeight) noexcept = 0;
        virtual void SendMouseButtonEvent(int a_button, bool a_down) noexcept = 0;
        virtual void SendMouseWheelEvent(int a_wheelDelta) noexcept = 0;

        virtual void Hide(std::string_view a_viewId) noexcept = 0;
        virtual void Tick() noexcept = 0;
    };
}
