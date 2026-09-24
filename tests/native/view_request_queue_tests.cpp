#include "Views/ViewRequestQueue.h"

#include <cassert>
#include <variant>

int main()
{
	OSFUI::ViewRequestQueue queue;
	queue.EnqueueOpen("acme/first");
	queue.EnqueueRelativePointer("acme/first", true);
	queue.Enqueue(OSFUI::ViewPresentationRequest::CloseAll);
	queue.EnqueueOpen("acme/second");
	const auto batch = queue.Take();
	assert(batch.presentation.size() == 4);
	assert(std::get<OSFUI::ViewRequestQueue::OpenRequest>(batch.presentation[0]).view == "acme/first");
	assert(std::get<OSFUI::ViewRequestQueue::RelativePointerRequest>(batch.presentation[1]).active);
	assert(std::get<OSFUI::ViewPresentationRequest>(batch.presentation[2]) == OSFUI::ViewPresentationRequest::CloseAll);
	assert(std::get<OSFUI::ViewRequestQueue::OpenRequest>(batch.presentation[3]).view == "acme/second");
	assert(queue.Take().presentation.empty());
}
