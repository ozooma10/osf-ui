#pragma once

#include "ViewManifest.h"
#include "ViewPresentationEvent.h"

#include <cstdint>
#include <vector>

namespace Runtime
{
    struct ViewShowResult
    {
        bool accepted{ false };
        bool documentCreated{ false };

        explicit operator bool() const noexcept { return accepted; }
    };

    class IViewPresenter
    {
    public:
        virtual ~IViewPresenter() = default;

        // accepted means the opening request was issued. documentCreated tells protocol owners to arm a fresh document before the next backend Tick.
        // Input and pause policy still wait for a matching Ready event.
        virtual ViewShowResult Show(const ViewManifest& a_view) noexcept = 0;

        // Reports whether the native focus transition was issued successfully.
        // A failed acquisition is fatal to an input-capturing presentation; a failed release may be retried while game input is already restored.
        virtual bool SetInputFocus(bool a_focused) noexcept = 0;
        // Game-window input can arrive outside the main thread. Implementations must synchronize any shared state they touch.
        virtual void SendKeyEvent(std::uint32_t a_virtualKey, bool a_down) noexcept = 0;
        virtual void UpdateMousePosition(int a_clientX, int a_clientY, int a_clientWidth, int a_clientHeight) noexcept = 0;
        virtual void SendMouseButtonEvent(int a_button, bool a_down) noexcept = 0;
        virtual void SendMouseWheelEvent(int a_wheelDelta) noexcept = 0;

        virtual void Hide(std::string_view a_viewId) noexcept = 0;

        // Advances the backend. NotReady revokes an earlier Ready state while replacement document is loading. Ready means the main document loaded successfully and a newer frame containing it was submitted.
        // ViewFailed closes one view; PresenterFailed is terminal and closes every view.
        virtual void Tick() noexcept = 0;
        virtual std::vector<ViewPresentationEvent> TakePresentationEvents() = 0;
    };
}
