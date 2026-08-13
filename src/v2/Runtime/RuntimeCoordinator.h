#pragma once

#include "IViewPresenter.h"
#include "ViewDiscovery.h"
#include "ViewRuntime.h"

#include <atomic>
#include <cstddef>
#include <filesystem>
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

        explicit RuntimeCoordinator(RegisterPapyrus a_registerPapyrus, IViewPresenter* a_viewPresenter = nullptr) noexcept;

        ViewLoadReport LoadViews(const std::filesystem::path& a_viewsDirectory);

        void NotifyDataLoaded() noexcept;
        void Tick();

        ViewRuntime& Views() noexcept;
        const ViewRuntime& Views() const noexcept;

    private:
        void TickPapyrusRegistration();
        void DispatchPresentationCommands();


        ViewRuntime _views;
        RegisterPapyrus _registerPapyrus{ nullptr };
        IViewPresenter* _viewPresenter{ nullptr };

        std::atomic_bool _dataLoadPending{ false };

        // Read and written only by Tick() on the main thread.
        bool _papyrusRegistered{ false };
    };
}