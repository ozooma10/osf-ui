#pragma once

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

        explicit RuntimeCoordinator(RegisterPapyrus a_registerPapyrus) noexcept;

        ViewLoadReport LoadViews(const std::filesystem::path& a_viewsDirectory);

        void NotifyDataLoaded() noexcept;
        void Tick();

        ViewRuntime& Views() noexcept;
        const ViewRuntime& Views() const noexcept;

    private:
        ViewRuntime _views;
        RegisterPapyrus _registerPapyrus{ nullptr };

        std::atomic_bool _dataLoadPending{ false };

        // Read and written only by Tick() on the main thread.
        bool _papyrusRegistered{ false };
    };
}