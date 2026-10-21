#include "../../tools/browser_shared/BrowserBoundedQueue.h"

#include "check.h"

#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdint>
#include <string>
#include <thread>
#include <vector>

using osfui::browser::BoundedQueue;
using osfui::browser::GameMessageCoalesceKey;
using namespace std::chrono_literals;

int main()
{
	using Queue = BoundedQueue<int>;
	using Result = Queue::PushResult;

	CHECK(GameMessageCoalesceKey("mouse", "move") == "mouse.move");
	CHECK(GameMessageCoalesceKey("mouse", "button").empty());
	CHECK(GameMessageCoalesceKey("resize") == "resize");
	CHECK(GameMessageCoalesceKey("focus") == "focus");
	CHECK(GameMessageCoalesceKey("browserAudio") == "browserAudio");
	CHECK(GameMessageCoalesceKey("setHidden", {}, "view-a") == "setHidden:view-a");
	CHECK(GameMessageCoalesceKey("frameAck").empty());
	CHECK(GameMessageCoalesceKey("postWeb").empty());

	Queue queue(3);
	CHECK(queue.Push(1, "resize") == Result::Queued);
	CHECK(queue.Push(2, "resize") == Result::Coalesced);
	CHECK(queue.Size() == 1);

	Queue::Item item;
	CHECK(queue.TryPop(item));
	CHECK(item.value == 2);

	CHECK(queue.Push(10, "mouse.move") == Result::Queued);
	CHECK(queue.Push(20) == Result::Queued);
	CHECK(queue.Push(30, "mouse.move") == Result::Queued);
	CHECK(queue.Push(40) == Result::Full);

	std::vector<Queue::Item> prefix{
		{ 1, {} },
		{ 2, {} },
	};
	CHECK(!queue.Prepend(std::move(prefix)));
	queue.Clear();
	prefix = {
		{ 1, {} },
		{ 2, {} },
	};
	CHECK(queue.Push(3) == Result::Queued);
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

	// Delayed audio updates retain only the latest consecutive pose, including
	// its original absolute expiry. Commands (mute/navigation/stop) are barriers.
	struct AudioUpdate { int pose; std::uint64_t expiresMs; };
	using AudioQueue = BoundedQueue<AudioUpdate>;
	using AudioResult = AudioQueue::PushResult;
	AudioQueue audio(3);
	const auto audioKey = GameMessageCoalesceKey("browserAudio");
	CHECK(audio.Push({1, 1000}, audioKey) == AudioResult::Queued);
	CHECK(audio.Push({2, 1050}, audioKey) == AudioResult::Coalesced);
	CHECK(audio.Push({0, 0}, GameMessageCoalesceKey("browserCommand")) == AudioResult::Queued);
	CHECK(audio.Push({3, 1100}, audioKey) == AudioResult::Queued);
	CHECK(audio.Push({4, 1150}, audioKey) == AudioResult::Coalesced);
	CHECK(audio.Size() == 3);
	AudioQueue::Item audioItem;
	CHECK(audio.TryPop(audioItem) && audioItem.value.pose == 2 && audioItem.value.expiresMs == 1050);
	CHECK(audio.TryPop(audioItem) && audioItem.value.pose == 0);
	CHECK(audio.TryPop(audioItem) && audioItem.value.pose == 4 && audioItem.value.expiresMs == 1150);

	std::fprintf(stderr, "wv2_bounded_queue_tests: %d checks, %d failure(s)\n",
		g_checks, g_failures);
	return g_failures;
}
