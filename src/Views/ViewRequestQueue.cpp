#include "ViewRequestQueue.h"

void OSFUI::ViewRequestQueue::Enqueue(ViewPresentationRequest a_request)
{
    std::lock_guard<std::mutex> lock(m_mutex);
    m_presentation.push_back(a_request);
}

void OSFUI::ViewRequestQueue::EnqueueView(std::string a_viewId, bool a_open, std::uint64_t a_requestId)
{
    std::lock_guard<std::mutex> lock(m_mutex);
    m_presentation.emplace_back(ViewRequest{
        .view = std::move(a_viewId),
        .open = a_open,
        .requestId = a_requestId,
    });
}

void OSFUI::ViewRequestQueue::EnqueueLauncherOpen(std::string a_viewId, std::uint64_t a_requestId)
{
    std::lock_guard<std::mutex> lock(m_mutex);
    m_presentation.emplace_back(LauncherRequest{ std::move(a_viewId), a_requestId });
}

void OSFUI::ViewRequestQueue::EnqueueRelativePointer(std::string a_viewId, bool a_active)
{
    std::lock_guard<std::mutex> lock(m_mutex);
    m_presentation.emplace_back(RelativePointerRequest{
        .view = std::move(a_viewId),
        .active = a_active,
    });
}

void OSFUI::ViewRequestQueue::EnqueueBackUnhandled(std::string a_viewId, std::uint64_t a_epoch)
{
    std::lock_guard<std::mutex> lock(m_mutex);
    m_presentation.emplace_back(BackUnhandled{ std::move(a_viewId), a_epoch });
}

std::vector<OSFUI::ViewRequestQueue::Operation> OSFUI::ViewRequestQueue::Take()
{
    std::lock_guard<std::mutex> lock(m_mutex);
    std::vector<Operation> batch;
    batch.swap(m_presentation);

    return batch;
}
