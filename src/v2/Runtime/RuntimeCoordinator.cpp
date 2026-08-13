#include "RuntimeCoordinator.h"

namespace Runtime
{
    RuntimeCoordinator::RuntimeCoordinator(RegisterPapyrus a_registerPapyrus) noexcept 
        : _registerPapyrus(a_registerPapyrus)
    {}

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
        if (_papyrusRegistered) {
            return;
        }

        _dataLoadPending.store(true, std::memory_order_release);
    }

    void RuntimeCoordinator::Tick()
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

        // GameVM may be temporarily unavailable. Keep the work pending so the next main-thread Tick retries instead of losing initialization.
        _dataLoadPending.store(true, std::memory_order_release);
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