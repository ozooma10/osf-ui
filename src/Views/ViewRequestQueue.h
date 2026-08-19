#pragma once

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
        struct Batch
        {
            std::vector<ViewPresentationRequest> presentation;
            std::vector<std::string> openViews;
        };

        void Enqueue(ViewPresentationRequest a_request);
        void EnqueueOpen(std::string a_viewId);
        Batch Take();

    private:
        std::mutex m_mutex;
        std::vector<ViewPresentationRequest> m_presentation;
        std::vector<std::string> m_openViews;
    };
}