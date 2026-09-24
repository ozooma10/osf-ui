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

        using Operation = std::variant<ViewPresentationRequest, OpenRequest, RelativePointerRequest>;

        struct Batch
        {
            std::vector<Operation> presentation;
        };

        void Enqueue(ViewPresentationRequest a_request);
        void EnqueueOpen(std::string a_viewId);
        void EnqueueRelativePointer(std::string a_viewId, bool a_active);
        Batch Take();

    private:
        std::mutex m_mutex;
        std::vector<Operation> m_presentation;
    };
}
