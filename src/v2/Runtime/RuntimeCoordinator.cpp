#include "RuntimeCoordinator.h"

namespace Runtime
{
    RuntimeCoordinator::RuntimeCoordinator(RegisterPapyrus a_registerPapyrus, IViewPresenter* a_viewPresenter) noexcept
        : _registerPapyrus(a_registerPapyrus), _viewPresenter(a_viewPresenter) {}

    ViewLoadReport RuntimeCoordinator::LoadViews(const std::filesystem::path& a_viewsDirectory)
    {
        auto discovery = DiscoverViews(a_viewsDirectory);

        ViewLoadReport report{
            .loaded = discovery.views.size(),
            .issues = std::move(discovery.issues)
        };

        _views.ReplaceViews(std::move(discovery.views));
        return report;
    }

    bool RuntimeCoordinator::RequestOpenView(std::string a_viewId)
    {
        return QueueViewRequest(ViewRequestAction::Open, std::move(a_viewId));
    }

    bool RuntimeCoordinator::RequestCloseView(std::string a_viewId)
    {
        return QueueViewRequest(ViewRequestAction::Close, std::move(a_viewId));
    }

    bool RuntimeCoordinator::QueueViewRequest(ViewRequestAction a_action, std::string a_viewId)
    {
        if (a_viewId.empty()) {
            return false;
        }

        std::scoped_lock lock{ _viewRequestsMutex };

        if (_pendingViewRequests.size() >= kMaxPendingViewRequests) {
            return false;
        }

        _pendingViewRequests.push_back({
            .action = a_action,
            .viewId = std::move(a_viewId)
        });

        return true;
    }

    std::vector<ViewRequest> RuntimeCoordinator::TakeViewRequests()
    {
        std::vector<ViewRequest> requests;

        {
            std::scoped_lock lock{ _viewRequestsMutex };
            requests.swap(_pendingViewRequests);
        }

        return requests;
    }

    void RuntimeCoordinator::ApplyViewRequests()
    {
        for (const auto& request : TakeViewRequests()) {
            const bool open = request.action == ViewRequestAction::Open;
            const auto result = open
                ? _views.OpenView(request.viewId)
                : _views.CloseView(request.viewId);

            if (result == ViewOperationResult::UnknownView) {
                REX::WARN(
                    "RuntimeCoordinator: ignored {} request for unknown view '{}'",
                    open ? "open" : "close",
                    request.viewId);
            }
        }
    }

    void RuntimeCoordinator::NotifyDataLoaded() noexcept
    {
        _dataLoadPending.store(true, std::memory_order_release);
    }

    void RuntimeCoordinator::Tick()
    {
        TickPapyrusRegistration();
        ApplyViewRequests();
        DispatchPresentationCommands();

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

                if (!_viewPresenter->Show(command.view)) {
                    // Show failed. Close the desired view so it cannot retain invisible input or pause policy. CloseView queues a Hide command, which the next loop drains.
                    _views.CloseView(command.view.id);
                }
            }
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
