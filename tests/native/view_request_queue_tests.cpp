#include "Views/ViewRequestQueue.h"

#include <cassert>
#include <variant>

int main()
{
	OSFUI::ViewRequestQueue queue;
	queue.EnqueueView("acme/first", true);
	queue.EnqueueRelativePointer("acme/first", true);
	queue.Enqueue(OSFUI::ViewPresentationRequest::CloseAll);
	queue.EnqueueView("acme/second", true);
	queue.EnqueueView("acme/second", false);
	const auto batch = queue.Take();
	assert(batch.size() == 5);
	assert(std::get<OSFUI::ViewRequestQueue::ViewRequest>(batch[0]).view == "acme/first");
	assert(std::get<OSFUI::ViewRequestQueue::RelativePointerRequest>(batch[1]).active);
	assert(std::get<OSFUI::ViewPresentationRequest>(batch[2]) == OSFUI::ViewPresentationRequest::CloseAll);
	assert(std::get<OSFUI::ViewRequestQueue::ViewRequest>(batch[3]).view == "acme/second");
	assert(!std::get<OSFUI::ViewRequestQueue::ViewRequest>(batch[4]).open);
	assert(queue.Take().empty());

	// Browser Back replies remain ordered with close/reopen requests and keep
	// their original presentation identity for the runtime's stale-reply check.
	queue.EnqueueView("acme/first", false);
	queue.EnqueueBackUnhandled("acme/first", 42);
	queue.EnqueueView("acme/second", true);
	const auto backBatch = queue.Take();
	assert(backBatch.size() == 3);
	const auto& back = std::get<OSFUI::ViewRequestQueue::BackUnhandled>(backBatch[1]);
	assert(back.view == "acme/first" && back.presentationEpoch == 42);
	assert(!std::get<OSFUI::ViewRequestQueue::ViewRequest>(backBatch[0]).open);
	assert(std::get<OSFUI::ViewRequestQueue::ViewRequest>(backBatch[2]).open);

	// Producers may enqueue during consumption without extending or invalidating
	// the batch being processed (ready and lifecycle callbacks do this).
	for (const auto& operation : batch) {
		if (const auto* view = std::get_if<OSFUI::ViewRequestQueue::ViewRequest>(&operation); view && view->open) {
			queue.EnqueueView(view->view, false);
		}
	}
	const auto next = queue.Take();
	assert(next.size() == 2);
	assert(std::get<OSFUI::ViewRequestQueue::ViewRequest>(next[0]).view == "acme/first");
	assert(std::get<OSFUI::ViewRequestQueue::ViewRequest>(next[1]).view == "acme/second");
	assert(queue.Take().empty());

	// A launcher preload is ordered with the open that follows it once Settings hands off.
	queue.EnqueuePreload("acme/first");
	queue.EnqueueView("acme/first", true);
	const auto preload = queue.Take();
	assert(preload.size() == 2);
	assert(std::get<OSFUI::ViewRequestQueue::ViewPreloadRequest>(preload[0]).view == "acme/first");
	assert(std::get<OSFUI::ViewRequestQueue::ViewRequest>(preload[1]).open);
	assert(queue.Take().empty());
}
