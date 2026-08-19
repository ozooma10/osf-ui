#include "ViewRequestQueue.h"

void OSFUI::ViewRequestQueue::Enqueue(ViewPresentationRequest a_request)
{
    std::lock_guard<std::mutex> lock(m_mutex);
    m_presentation.push_back(a_request);
}

void OSFUI::ViewRequestQueue::EnqueueOpen(std::string a_viewId)
{
    std::lock_guard<std::mutex> lock(m_mutex);
    m_openViews.push_back({
        .view = std::move(a_viewId),
        .requestedAt = std::chrono::steady_clock::now(),
    });
}

OSFUI::ViewRequestQueue::Batch OSFUI::ViewRequestQueue::Take()
{
    std::lock_guard<std::mutex> lock(m_mutex);
    Batch batch;

    batch.presentation.swap(m_presentation);
    batch.openViews.swap(m_openViews);

    return batch;
}
