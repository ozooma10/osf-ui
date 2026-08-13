#pragma once

#include "IViewPresenter.h"
#include "ViewDiscovery.h"
#include "ViewRequest.h"
#include "ViewRuntime.h"

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <mutex>
#include <string>
#include <string_view>
#include <unordered_set>
#include <vector>

namespace Runtime
{
    struct ViewLoadReport
    {
        std::size_t loaded{ 0 };
        std::vector<ViewDiscoveryIssue> issues;
    };

    class RuntimeCoordinator
    {
    public:
        using RegisterPapyrus = bool (*)();
        using ApplyInputCapture = void (*)(bool);
        using ApplyGamePause = void (*)(bool);

        explicit RuntimeCoordinator(RegisterPapyrus a_registerPapyrus, IViewPresenter* a_viewPresenter = nullptr, ApplyInputCapture a_applyInputCapture = nullptr, ApplyGamePause a_applyGamePause = nullptr) noexcept;

        ViewLoadReport LoadViews(const std::filesystem::path& a_viewsDirectory);
        void EnableInputRouting() noexcept;

        bool RequestOpenView(std::string a_viewId);
        bool RequestCloseView(std::string a_viewId);
        void NotifyMenuOpenClose(std::string_view a_menuName, bool a_opening);

        bool IsInputCaptured() const noexcept;

        bool RouteKeyEvent(std::uint32_t a_virtualKey, bool a_down) noexcept;
        void RouteMousePosition(int a_clientX, int a_clientY, int a_clientWidth, int a_clientHeight) noexcept;
        void RouteMouseButtonEvent(int a_button, bool a_down) noexcept;
        void RouteMouseWheelEvent(int a_wheelDelta) noexcept;

        void NotifyDataLoaded() noexcept;
        void Tick();

        ViewRuntime& Views() noexcept;
        const ViewRuntime& Views() const noexcept;

    private:
        static constexpr std::size_t kMaxPendingViewRequests = 256;

        bool QueueViewRequest(ViewRequestAction a_action, std::string a_viewId);

        std::vector<ViewRequest> TakeViewRequests();
        void ApplyViewRequests();
        void MarkViewInstantiated(std::string_view a_viewId);

        void TickPapyrusRegistration();
        void DispatchPresentationCommands();
        void ApplyPresentationEvents();
        bool IsViewReady(std::string_view a_viewId) const;

        void ApplyFrameworkInputActions();
        bool ReconcileInputFocus();
        void ApplyInputCapturePolicy();
        void ApplyGamePausePolicy();

        ViewRuntime _views;

        std::mutex _viewRequestsMutex;
        std::vector<ViewRequest> _pendingViewRequests;
        std::unordered_set<std::string> _knownViewIds;
        std::unordered_set<std::string> _instantiatedViewIds;
        std::unordered_set<std::string> _readyViewIds;
        bool _loadingMenuOpen{ false };
        bool _mainMenuOpen{ false };

        RegisterPapyrus _registerPapyrus{ nullptr };
        IViewPresenter* _viewPresenter{ nullptr };
        ApplyInputCapture _applyInputCapture{ nullptr };
        ApplyGamePause _applyGamePause{ nullptr };

        std::atomic_bool _dataLoadPending {false};

        std::atomic_bool _inputRoutingAvailable {false};

        // Published by Tick for the game-window thread
        std::atomic_bool _inputCaptured {false};

        // Latched by the game-window thread and drained by Tick
        std::atomic_bool _escapeClosePending {false};

        // Read and written only by Tick() on the main thread.
        bool _papyrusRegistered{ false };
        bool _inputFocusRequested{ false };
    };
}
