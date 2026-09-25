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
        struct ViewRequest
        {
            std::string                           view;
            bool                                  open;
            std::chrono::steady_clock::time_point requestedAt;
        };

        struct RelativePointerRequest
        {
            std::string view;
            bool        active{ false };
        };

        using Operation = std::variant<ViewPresentationRequest, ViewRequest, RelativePointerRequest>;

        void Enqueue(ViewPresentationRequest a_request);
        void EnqueueView(std::string a_viewId, bool a_open);
        void EnqueueRelativePointer(std::string a_viewId, bool a_active);
        // One finite FIFO batch. Callback-enqueued work stays for the next take.
        std::vector<Operation> Take();

    private:
        std::mutex m_mutex;
        std::vector<Operation> m_presentation;
    };
}
