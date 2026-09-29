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
            std::uint64_t requestId{}; // Nonzero activates only this launcher's retained frame.
        };

        struct LauncherRequest
        {
            std::string view;
            std::uint64_t requestId;
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

        using Operation = std::variant<ViewPresentationRequest, ViewRequest, LauncherRequest, RelativePointerRequest, BackUnhandled>;

        void Enqueue(ViewPresentationRequest a_request);
        void EnqueueView(std::string a_viewId, bool a_open, std::uint64_t a_requestId = 0);
        void EnqueueLauncherOpen(std::string a_viewId, std::uint64_t a_requestId);
        void EnqueueRelativePointer(std::string a_viewId, bool a_active);
        void EnqueueBackUnhandled(std::string a_viewId, std::uint64_t a_epoch);
        // One finite FIFO batch. Callback-enqueued work stays for the next take.
        std::vector<Operation> Take();

    private:
        std::mutex m_mutex;
        std::vector<Operation> m_presentation;
    };
}
