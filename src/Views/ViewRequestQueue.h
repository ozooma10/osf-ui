#pragma once

#include <chrono>
#include <vector>
#include <string>
#include <mutex>
#include <variant>

namespace OSFUI
{
    enum class ViewPresentationRequest
    {
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
            std::vector<std::variant<ViewPresentationRequest, OpenRequest, RelativePointerRequest>> presentation;
        };

        void Enqueue(ViewPresentationRequest a_request);
        void EnqueueOpen(std::string a_viewId);
        void EnqueueRelativePointer(std::string a_viewId, bool a_active);
        Batch Take();

    private:
        std::mutex m_mutex;
        std::vector<std::variant<ViewPresentationRequest, OpenRequest, RelativePointerRequest>> m_presentation;
    };
}
