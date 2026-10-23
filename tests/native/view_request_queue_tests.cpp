#include "Views/ViewRequestQueue.h"

#include "check.h"
#include <variant>

int main()
{
	OSFUI::ViewRequestQueue queue;
	queue.EnqueueView("acme/first", true);
	queue.EnqueueRelativePointer("acme/first", true);
	queue.Enqueue(OSFUI::ViewPresentationRequest::Back);
	queue.EnqueueView("acme/second", true);
	queue.EnqueueView("acme/second", false);
	const auto batch = queue.Take();
	CHECK(batch.size() == 5);
	CHECK(std::get<OSFUI::ViewRequestQueue::ViewRequest>(batch[0]).view == "acme/first");
	CHECK(std::get<OSFUI::ViewRequestQueue::RelativePointerRequest>(batch[1]).active);
	CHECK(std::get<OSFUI::ViewPresentationRequest>(batch[2]) == OSFUI::ViewPresentationRequest::Back);
	CHECK(std::get<OSFUI::ViewRequestQueue::ViewRequest>(batch[3]).view == "acme/second");
	CHECK(!std::get<OSFUI::ViewRequestQueue::ViewRequest>(batch[4]).open);
	CHECK(queue.Take().empty());

	// Keep Back replies ordered with close/reopen requests; retain IDs to detect stale replies.
	queue.EnqueueView("acme/first", false);
	queue.EnqueueBackUnhandled("acme/first", 42);
	queue.EnqueueView("acme/second", true);
	const auto backBatch = queue.Take();
	CHECK(backBatch.size() == 3);
	const auto& back = std::get<OSFUI::ViewRequestQueue::BackUnhandled>(backBatch[1]);
	CHECK(back.view == "acme/first" && back.presentationEpoch == 42);
	CHECK(!std::get<OSFUI::ViewRequestQueue::ViewRequest>(backBatch[0]).open);
	CHECK(std::get<OSFUI::ViewRequestQueue::ViewRequest>(backBatch[2]).open);

	// Work queued by callbacks waits for the next batch.
	for (const auto& operation : batch) {
		if (const auto* view = std::get_if<OSFUI::ViewRequestQueue::ViewRequest>(&operation); view && view->open) {
			queue.EnqueueView(view->view, false);
		}
	}
	const auto next = queue.Take();
	CHECK(next.size() == 2);
	CHECK(std::get<OSFUI::ViewRequestQueue::ViewRequest>(next[0]).view == "acme/first");
	CHECK(std::get<OSFUI::ViewRequestQueue::ViewRequest>(next[1]).view == "acme/second");
	CHECK(queue.Take().empty());
	return g_failures ? 1 : 0;
}
