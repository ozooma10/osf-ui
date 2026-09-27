#pragma once

#include <vector>
#include <string>
#include <mutex>
#include <variant>
#include <cstdint>

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
            std::string view;
            bool        open;
        };

        struct RelativePointerRequest
        {
            std::string view;
            bool        active{ false };
        };

        struct BackUnhandled
        {
            std::string view;
            std::uint64_t presentationEpoch{};
        };

        using Operation = std::variant<ViewPresentationRequest, ViewRequest, RelativePointerRequest, BackUnhandled>;

        void Enqueue(ViewPresentationRequest a_request);
        void EnqueueView(std::string a_viewId, bool a_open);
        void EnqueueRelativePointer(std::string a_viewId, bool a_active);
        void EnqueueBackUnhandled(std::string a_viewId, std::uint64_t a_epoch);
        // One finite FIFO batch. Callback-enqueued work stays for the next take.
        std::vector<Operation> Take();

    private:
        std::mutex m_mutex;
        std::vector<Operation> m_presentation;
    };
}
