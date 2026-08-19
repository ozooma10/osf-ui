#pragma once

#include <chrono>
#include <vector>
#include <string>
#include <mutex>

namespace OSFUI
{
    enum class ViewPresentationRequest
    {
        ToggleDefault,
        Back,
        CloseAll
    };

    class ViewRequestQueue
    {
    public:
        struct OpenRequest
        {
            std::string                           view;
            std::chrono::steady_clock::time_point requestedAt;
        };

        struct Batch
        {
            std::vector<ViewPresentationRequest> presentation;
            std::vector<OpenRequest> openViews;
        };

        void Enqueue(ViewPresentationRequest a_request);
        void EnqueueOpen(std::string a_viewId);
        Batch Take();

    private:
        std::mutex m_mutex;
        std::vector<ViewPresentationRequest> m_presentation;
        std::vector<OpenRequest> m_openViews;
    };
}
