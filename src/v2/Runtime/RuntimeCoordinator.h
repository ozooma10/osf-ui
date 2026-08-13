#pragma once

#include "IViewPresenter.h"
#include "ViewDiscovery.h"
#include "ViewRequest.h"
#include "ViewRuntime.h"

#include <atomic>
#include <cstddef>
#include <filesystem>
#include <mutex>
#include <string>
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

        bool RequestOpenView(std::string a_viewId);
        bool RequestCloseView(std::string a_viewId);

        void NotifyDataLoaded() noexcept;
        void Tick();

        ViewRuntime& Views() noexcept;
        const ViewRuntime& Views() const noexcept;

    private:
        static constexpr std::size_t kMaxPendingViewRequests = 256;

        bool QueueViewRequest(ViewRequestAction a_action, std::string a_viewId);

        std::vector<ViewRequest> TakeViewRequests();
        void ApplyViewRequests();

        void TickPapyrusRegistration();
        void DispatchPresentationCommands();

        ViewRuntime _views;

        std::mutex _viewRequestsMutex;
        std::vector<ViewRequest> _pendingViewRequests;

        RegisterPapyrus _registerPapyrus{ nullptr };
        IViewPresenter* _viewPresenter{ nullptr };

        std::atomic_bool _dataLoadPending{ false };

        // Read and written only by Tick() on the main thread.
        bool _papyrusRegistered{ false };
    };
}
