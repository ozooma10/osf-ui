#include "../../tools/webview2_shared/Wv2BoundedQueue.h"

#include "check.h"

#include <atomic>
#include <chrono>
#include <cstdio>
#include <string>
#include <thread>
#include <vector>

using osfui::wv2::BoundedQueue;
using osfui::wv2::GameMessageCoalesceKey;
using namespace std::chrono_literals;

int main()
{
	using Queue = BoundedQueue<int>;
	using Result = Queue::PushResult;

	CHECK(GameMessageCoalesceKey("mouse", "move") == "mouse.move");
	CHECK(GameMessageCoalesceKey("mouse", "button").empty());
	CHECK(GameMessageCoalesceKey("resize") == "resize");
	CHECK(GameMessageCoalesceKey("setHidden", {}, "view-a") == "setHidden:view-a");
	CHECK(GameMessageCoalesceKey("frameAck").empty());
	CHECK(GameMessageCoalesceKey("postWeb").empty());

	Queue queue(3);
	CHECK(queue.Push(1, "resize", 1) == Result::Queued);
	CHECK(queue.Push(2, "resize", 2) == Result::Coalesced);
	CHECK(queue.Size() == 1);

	Queue::Item item;
	CHECK(queue.TryPop(item));
	CHECK(item.value == 2);
	CHECK(item.sequence == 2);

	CHECK(queue.Push(10, "mouse.move") == Result::Queued);
	CHECK(queue.Push(20) == Result::Queued);
	CHECK(queue.Push(30, "mouse.move") == Result::Queued);
	CHECK(queue.Push(40) == Result::Full);

	std::vector<Queue::Item> prefix{
		{ 1, {}, 1 },
		{ 2, {}, 2 },
	};
	CHECK(!queue.Prepend(std::move(prefix)));
	queue.Clear();
	prefix = {
		{ 1, {}, 1 },
		{ 2, {}, 2 },
	};
	CHECK(queue.Push(3, {}, 3) == Result::Queued);
	CHECK(queue.Prepend(std::move(prefix)));
	CHECK(queue.TryPop(item) && item.value == 1);
	CHECK(queue.TryPop(item) && item.value == 2);
	CHECK(queue.TryPop(item) && item.value == 3);

	std::atomic_bool waiterReturned{ false };
	std::thread waiter([&] {
		Queue::Item ignored;
		CHECK(!queue.WaitPop(ignored));
		waiterReturned.store(true);
	});
	std::this_thread::sleep_for(10ms);
	queue.Close();
	waiter.join();
	CHECK(waiterReturned.load());
	CHECK(queue.Push(9) == Result::Closed);

	queue.Reset();
	CHECK(queue.Push(9) == Result::Queued);
	CHECK(queue.TryPop(item) && item.value == 9);

	std::fprintf(stderr, "wv2_bounded_queue_tests: %d checks, %d failure(s)\n",
		g_checks, g_failures);
	return g_failures;
}
