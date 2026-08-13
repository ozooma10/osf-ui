#include "RuntimeCoordinator.h"

#include "v2/Bridge/BridgeRuntime.h"

namespace Runtime
{
    RuntimeCoordinator::RuntimeCoordinator(RegisterPapyrus a_registerPapyrus, IViewPresenter* a_viewPresenter, ApplyInputCapture a_applyInputCapture, ApplyGamePause a_applyGamePause, Bridge::IViewMessageTransport* a_messageTransport)
        : _registerPapyrus(a_registerPapyrus), _viewPresenter(a_viewPresenter), _applyInputCapture(a_applyInputCapture), _applyGamePause(a_applyGamePause)
    {
        if (a_messageTransport) {
            _bridge = std::make_unique<Bridge::BridgeRuntime>(*a_messageTransport, [this](ViewRequestAction a_action, std::string a_viewId) {
                return QueueViewRequest(a_action, std::move(a_viewId));
            });
        }
    }

    RuntimeCoordinator::~RuntimeCoordinator() = default;

    ViewLoadReport RuntimeCoordinator::LoadViews(const std::filesystem::path& a_viewsDirectory)
    {
        auto discovery = DiscoverViews(a_viewsDirectory);

        ViewLoadReport report {.loaded = discovery.views.size(), .issues = std::move(discovery.issues)};

        std::unordered_set<std::string> knownViewIds;
        std::unordered_set<std::string> inputCapturingViewIds;

        for (const auto& view : discovery.views) {
            knownViewIds.insert(view.id);
            if (view.capturesInput) {
                inputCapturingViewIds.insert(view.id);
            }
        }

        if (_bridge) {
            _bridge->ResetDocuments();
        }

        {
            std::scoped_lock lock {_viewRequestsMutex};

            _views.ReplaceViews(std::move(discovery.views));
            _knownViewIds = std::move(knownViewIds);
            _inputCapturingViewIds = std::move(inputCapturingViewIds);
            _instantiatedViewIds.clear();
            _readyViewIds.clear();
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
        return QueueViewRequest(ViewRequestAction::Open, std::move(a_viewId)) == ViewRequestResult::Accepted;
    }

    bool RuntimeCoordinator::RequestCloseView(std::string a_viewId)
    {
        return QueueViewRequest(ViewRequestAction::Close, std::move(a_viewId)) == ViewRequestResult::Accepted;
    }

    void RuntimeCoordinator::NotifyMenuOpenClose(std::string_view a_menuName, bool a_opening)
    {
        const bool loadingMenu = a_menuName == "LoadingMenu";
        const bool mainMenu = a_menuName == "MainMenu";

        if (!loadingMenu && !mainMenu) {
            return;
        }

        std::scoped_lock lock {_viewRequestsMutex};

        if (loadingMenu) {
            _loadingMenuOpen = a_opening;
        } else {
            _mainMenuOpen = a_opening;
        }

        if (a_opening) {
            // A lifecycle close is a barrier: discard stale opens queued before the transition and reserve the queue for the mandatory close.
            _pendingViewRequests.clear();
            _pendingViewRequests.push_back({.action = ViewRequestAction::CloseAll});
        }
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

    ViewRequestResult RuntimeCoordinator::QueueViewRequest(ViewRequestAction a_action, std::string a_viewId)
    {
        if (a_viewId.empty()) {
            return ViewRequestResult::InvalidViewId;
        }

        std::scoped_lock lock {_viewRequestsMutex};

        if (a_action == ViewRequestAction::Open) {
            if (_loadingMenuOpen || _mainMenuOpen) {
                return ViewRequestResult::BlockedByGameMenu;
            }

            if (!_knownViewIds.contains(a_viewId)) {
                return ViewRequestResult::UnknownView;
            }

            if (_inputCapturingViewIds.contains(a_viewId) && !_inputRoutingAvailable.load(std::memory_order_acquire)) {
                return ViewRequestResult::InputUnavailable;
            }
        } else if (!_instantiatedViewIds.contains(a_viewId)) {
            return ViewRequestResult::NotInstantiated;
        }

        if (_pendingViewRequests.size() >= kMaxPendingViewRequests) {
            return ViewRequestResult::QueueFull;
        }

        _pendingViewRequests.push_back({.action = a_action, .viewId = std::move(a_viewId)});

        return ViewRequestResult::Accepted;
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
            if (request.action == ViewRequestAction::CloseAll) {
                _views.CloseAllViews();
                continue;
            }

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

        if (_viewPresenter) {
            _viewPresenter->Tick();
            if (_bridge) {
                _bridge->Tick();
            }
            // Web messages are drained by presenter Tick and may enqueue view requests. Apply them now so page-driven close releases modal policy in this same main-thread tick.
            ApplyViewRequests();
            ApplyPresentationEvents();
            // A failed view or presenter closes logical state above. Deliver the resulting hides before reconciling engine input and pause policy.
            DispatchPresentationCommands();
        }

        const auto presentation = _views.Presentation();
        const bool activeMenuReady = presentation.activeMenu && IsViewReady(*presentation.activeMenu);
        const bool shouldCaptureInput = _viewPresenter && activeMenuReady && _inputRoutingAvailable.load(std::memory_order_acquire) && presentation.capturesInput;

        if (shouldCaptureInput) {
            ApplyInputCapturePolicy();
            if (!ReconcileInputFocus()) {
                // Focus acquisition is part of opening an input-capturing menu.
                // Roll back the logical open and every engine-side effect in this same main-thread tick when it cannot be established.
                DispatchPresentationCommands();
                ReconcileInputFocus();
                ApplyInputCapturePolicy();
            }
        } else {
            ReconcileInputFocus();
            ApplyInputCapturePolicy();
        }

        ApplyGamePausePolicy();
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
                    _readyViewIds.erase(command.view.id);
                    _viewPresenter->Hide(command.view.id);
                    continue;
                }

                _readyViewIds.erase(command.view.id);

                if (command.view.capturesInput && !_inputRoutingAvailable.load(std::memory_order_acquire)) {
                    REX::WARN("RuntimeCoordinator: refused to show input-capturing view '{}' because game-window input routing is unavailable", command.view.id);
                    _views.CloseView(command.view.id);
                    continue;
                }

                const auto result = _viewPresenter->Show(command.view);
                if (!result) {
                    // Show failed. Return logical state to closed and queue its defensive hide.
                    _views.CloseView(command.view.id);
                    continue;
                }

                if (result.documentCreated && _bridge) {
                    _bridge->OnDocumentCreated(command.view.id);
                }
                MarkViewInstantiated(command.view.id);
            }
        }
    }

    void RuntimeCoordinator::ApplyPresentationEvents()
    {
        if (!_viewPresenter) {
            return;
        }

        for (auto& event : _viewPresenter->TakePresentationEvents()) {
            switch (event.kind) {
            case ViewPresentationEventKind::NotReady:
                _readyViewIds.erase(event.viewId);
                break;
            case ViewPresentationEventKind::Ready: {
                const auto openViewIds = _views.Presentation().openViewIds;
                if (std::ranges::find(openViewIds, event.viewId) != openViewIds.end()) {
                    _readyViewIds.insert(std::move(event.viewId));
                }
                break;
            }
            case ViewPresentationEventKind::ViewFailed:
                _readyViewIds.erase(event.viewId);
                REX::ERROR("RuntimeCoordinator: presentation failed for view '{}': {}", event.viewId, event.detail);
                _views.CloseView(event.viewId);
                break;
            case ViewPresentationEventKind::PresenterFailed:
                _readyViewIds.clear();
                REX::ERROR("RuntimeCoordinator: presentation backend failed: {}", event.detail);
                _views.CloseAllViews();
                if (_bridge) {
                    _bridge->ResetDocuments();
                }
                {
                    std::scoped_lock lock {_viewRequestsMutex};
                    _instantiatedViewIds.clear();
                    _pendingViewRequests.clear();
                }
                break;
            }
        }
    }

    bool RuntimeCoordinator::IsViewReady(std::string_view a_viewId) const
    {
        return _readyViewIds.contains(std::string {a_viewId});
    }

    void RuntimeCoordinator::ApplyFrameworkInputActions()
    {
        if (_escapeClosePending.exchange(false, std::memory_order_acq_rel)) {
            _views.CloseActiveMenu();
        }
    }

    bool RuntimeCoordinator::ReconcileInputFocus()
    {
        if (!_viewPresenter) {
            return false;
        }

        const auto presentation = _views.Presentation();
        const bool activeMenuReady = presentation.activeMenu && IsViewReady(*presentation.activeMenu);
        const bool shouldFocus = activeMenuReady && _inputRoutingAvailable.load(std::memory_order_acquire) && presentation.capturesInput;

        if (_inputFocusRequested == shouldFocus) {
            return true;
        }

        if (!_viewPresenter->SetInputFocus(shouldFocus)) {
            if (shouldFocus) {
                REX::ERROR("RuntimeCoordinator: input focus acquisition failed; closing the active menu");
                _views.CloseActiveMenu();
            }
            return false;
        }

        _inputFocusRequested = shouldFocus;
        return true;
    }

    void RuntimeCoordinator::ApplyInputCapturePolicy()
    {
        const auto presentation = _views.Presentation();
        const bool activeMenuReady = presentation.activeMenu && IsViewReady(*presentation.activeMenu);
        const bool shouldCapture = _viewPresenter && activeMenuReady && _inputRoutingAvailable.load(std::memory_order_acquire) && presentation.capturesInput;
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

    void RuntimeCoordinator::ApplyGamePausePolicy()
    {
        const auto presentation = _views.Presentation();
        const bool activeMenuReady = presentation.activeMenu && IsViewReady(*presentation.activeMenu);
        const bool shouldPause = _viewPresenter != nullptr && activeMenuReady && presentation.pausesGame;

        if (_applyGamePause) {
            _applyGamePause(shouldPause);
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
