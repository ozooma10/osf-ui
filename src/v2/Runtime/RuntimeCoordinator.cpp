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

    void RuntimeCoordinator::NotifyDataLoaded() noexcept
    {
        _dataLoadPending.store(true, std::memory_order_release);
    }

    void RuntimeCoordinator::Tick()
    {
        TickPapyrusRegistration();
        DispatchPresentationCommands();
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