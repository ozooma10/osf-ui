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

        struct RelativePointerRequest
        {
            std::string view;
            bool        active{ false };
        };

        struct Batch
        {
            std::vector<ViewPresentationRequest> presentation;
            std::vector<OpenRequest> openViews;
            std::vector<RelativePointerRequest> relativePointer;
        };

        void Enqueue(ViewPresentationRequest a_request);
        void EnqueueOpen(std::string a_viewId);
        void EnqueueRelativePointer(std::string a_viewId, bool a_active);
        Batch Take();

    private:
        std::mutex m_mutex;
        std::vector<ViewPresentationRequest> m_presentation;
        std::vector<OpenRequest> m_openViews;
        std::vector<RelativePointerRequest> m_relativePointer;
    };
}
