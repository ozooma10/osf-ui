#include "RuntimeCoordinator.h"

namespace Runtime
{
    RuntimeCoordinator::RuntimeCoordinator(RegisterPapyrus a_registerPapyrus, IViewPresenter* a_viewPresenter, ApplyInputCapture a_applyInputCapture) noexcept
        : _registerPapyrus(a_registerPapyrus), _viewPresenter(a_viewPresenter), _applyInputCapture(a_applyInputCapture)
    {}

    ViewLoadReport RuntimeCoordinator::LoadViews(const std::filesystem::path& a_viewsDirectory)
    {
        auto discovery = DiscoverViews(a_viewsDirectory);

        ViewLoadReport report {.loaded = discovery.views.size(), .issues = std::move(discovery.issues)};

        std::unordered_set<std::string> knownViewIds;

        for (const auto& view : discovery.views) {
            knownViewIds.insert(view.id);
        }

        {
            std::scoped_lock lock {_viewRequestsMutex};

            _views.ReplaceViews(std::move(discovery.views));
            _knownViewIds = std::move(knownViewIds);
            _instantiatedViewIds.clear();
            _pendingViewRequests.clear();
        }

        return report;
    }

    void RuntimeCoordinator::EnableInputRouting() noexcept
    {
        _inputRoutingAvailable.store(true, std::memory_order_release);
    }

    bool RuntimeCoordinator::RequestOpenView(std::string a_viewId)
    {
        return QueueViewRequest(ViewRequestAction::Open, std::move(a_viewId));
    }

    bool RuntimeCoordinator::RequestCloseView(std::string a_viewId)
    {
        return QueueViewRequest(ViewRequestAction::Close, std::move(a_viewId));
    }

    bool RuntimeCoordinator::IsInputCaptured() const noexcept
    {
        return _inputCaptured.load(std::memory_order_acquire);
    }

    bool RuntimeCoordinator::RouteKeyEvent(std::uint32_t a_virtualKey, bool a_down) noexcept
    {
        if (!IsInputCaptured() || !_viewPresenter) {
            return false;
        }

        constexpr std::uint32_t kEscapeVirtualKey = 0x1B;

        if (a_virtualKey == kEscapeVirtualKey) {
            if (a_down) {
                _escapeClosePending.store(true, std::memory_order_release);
            }

            return true;
        }

        _viewPresenter->SendKeyEvent(a_virtualKey, a_down);
        return true;
    }

    void RuntimeCoordinator::RouteMousePosition(int a_clientX, int a_clientY, int a_clientWidth, int a_clientHeight) noexcept
    {
        if (IsInputCaptured() && _viewPresenter) {
            _viewPresenter->UpdateMousePosition(a_clientX, a_clientY, a_clientWidth, a_clientHeight);
        }
    }

    void RuntimeCoordinator::RouteMouseButtonEvent(int a_button, bool a_down) noexcept
    {
        if (IsInputCaptured() && _viewPresenter) {
            _viewPresenter->SendMouseButtonEvent(a_button, a_down);
        }
    }

    void RuntimeCoordinator::RouteMouseWheelEvent(int a_wheelDelta) noexcept
    {
        if (IsInputCaptured() && _viewPresenter) {
            _viewPresenter->SendMouseWheelEvent(a_wheelDelta);
        }
    }

    bool RuntimeCoordinator::QueueViewRequest(ViewRequestAction a_action, std::string a_viewId)
    {
        if (a_viewId.empty()) {
            return false;
        }

        std::scoped_lock lock {_viewRequestsMutex};

        if (a_action == ViewRequestAction::Open) {
            if (!_knownViewIds.contains(a_viewId)) {
                return false;
            }
        } else if (!_instantiatedViewIds.contains(a_viewId)) {
            return false;
        }

        if (_pendingViewRequests.size() >= kMaxPendingViewRequests) {
            return false;
        }

        _pendingViewRequests.push_back({.action = a_action, .viewId = std::move(a_viewId)});

        return true;
    }

    std::vector<ViewRequest> RuntimeCoordinator::TakeViewRequests()
    {
        std::vector<ViewRequest> requests;

        {
            std::scoped_lock lock {_viewRequestsMutex};
            requests.swap(_pendingViewRequests);
        }

        return requests;
    }

    void RuntimeCoordinator::ApplyViewRequests()
    {
        for (const auto& request : TakeViewRequests()) {
            const bool open = request.action == ViewRequestAction::Open;
            const auto result = open ? _views.OpenView(request.viewId) : _views.CloseView(request.viewId);

            if (result == ViewOperationResult::UnknownView) {
                REX::WARN("RuntimeCoordinator: ignored {} request for unknown view '{}'", open ? "open" : "close", request.viewId);
            }
        }
    }

    void RuntimeCoordinator::MarkViewInstantiated(std::string_view a_viewId)
    {
        std::scoped_lock lock {_viewRequestsMutex};
        _instantiatedViewIds.emplace(a_viewId);
    }

    void RuntimeCoordinator::NotifyDataLoaded() noexcept
    {
        _dataLoadPending.store(true, std::memory_order_release);
    }

    void RuntimeCoordinator::Tick()
    {
        TickPapyrusRegistration();
        ApplyViewRequests();
        ApplyFrameworkInputActions();
        DispatchPresentationCommands();
        const bool shouldCaptureInput = _viewPresenter && _inputRoutingAvailable.load(std::memory_order_acquire) && _views.Presentation().capturesInput;

        if (shouldCaptureInput) {
            ApplyInputCapturePolicy();
            ReconcileInputFocus();
        } else {
            ReconcileInputFocus();
            ApplyInputCapturePolicy();
        }

        if (_viewPresenter) {
            _viewPresenter->Tick();
        }
    }

    void RuntimeCoordinator::TickPapyrusRegistration()
    {
        if (_papyrusRegistered) {
            _dataLoadPending.store(false, std::memory_order_release);
            return;
        }

        if (!_dataLoadPending.exchange(false, std::memory_order_acq_rel)) {
            return;
        }

        if (_registerPapyrus && _registerPapyrus()) {
            _papyrusRegistered = true;
            return;
        }

        // GameVM may be temporarily unavailable. Retry during the next main-thread tick instead of losing initialization.
        _dataLoadPending.store(true, std::memory_order_release);
    }

    void RuntimeCoordinator::DispatchPresentationCommands()
    {
        if (!_viewPresenter) {
            return;
        }

        while (true) {
            const auto commands = _views.TakePresentationCommands();

            if (commands.empty()) {
                return;
            }

            for (const auto& command : commands) {
                if (command.action == ViewPresentationAction::Hide) {
                    _viewPresenter->Hide(command.view.id);
                    continue;
                }

                if (command.view.capturesInput && !_inputRoutingAvailable.load(std::memory_order_acquire)) {
                    REX::WARN("RuntimeCoordinator: refused to show input-capturing view '{}' because game-window input routing is unavailable", command.view.id);
                    _views.CloseView(command.view.id);
                    continue;
                }

                if (!_viewPresenter->Show(command.view)) {
                    // Show failed. Return logical state to closed and queue its defensive hide.
                    _views.CloseView(command.view.id);
                    continue;
                }

                MarkViewInstantiated(command.view.id);
            }
        }
    }

    void RuntimeCoordinator::ApplyFrameworkInputActions()
    {
        if (_escapeClosePending.exchange(false, std::memory_order_acq_rel)) {
            _views.CloseActiveMenu();
        }
    }

    void RuntimeCoordinator::ReconcileInputFocus()
    {
        if (!_viewPresenter) {
            return;
        }

        const bool shouldFocus = _inputRoutingAvailable.load(std::memory_order_acquire) && _views.Presentation().capturesInput;

        if (_inputFocusRequested == shouldFocus) {
            return;
        }

        _viewPresenter->SetInputFocus(shouldFocus);
        _inputFocusRequested = shouldFocus;
    }

    void RuntimeCoordinator::ApplyInputCapturePolicy()
    {
        const bool shouldCapture = _viewPresenter && _inputRoutingAvailable.load(std::memory_order_acquire) && _views.Presentation().capturesInput;
        // Revoke routing before returning controls to the game. On acquisition, publish routing only after the game-side capture policy is applied.
        if (!shouldCapture) {
            _inputCaptured.store(false, std::memory_order_release);
        }

        if (_applyInputCapture) {
            _applyInputCapture(shouldCapture);
        }

        if (shouldCapture) {
            _inputCaptured.store(true, std::memory_order_release);
        }
    }

    ViewRuntime& RuntimeCoordinator::Views() noexcept
    {
        return _views;
    }

    const ViewRuntime& RuntimeCoordinator::Views() const noexcept
    {
        return _views;
    }
}
