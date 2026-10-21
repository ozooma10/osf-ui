#include "Render/BrowserHostRetirement.h"
#include "check.h"

#include <atomic>
#include <iostream>
#include <stdexcept>

int main()
{
	using OSFUI::BrowserHostRetirement;
	using Result = BrowserHostRetirement::Queue::PushResult;
	using namespace std::chrono_literals;
	BrowserHostRetirement retirement(3);
	std::promise<void> entered, release;
	const auto released = release.get_future().share();
	std::atomic_bool finished{};
	CHECK(retirement.Start([&] { entered.set_value(); released.wait(); finished = true; }));
	entered.get_future().wait();
	CHECK(retirement.Pending());
	CHECK(!retirement.TakeReady()); // Runtime polling never waits on the old host.
	CHECK(!retirement.Start([] { throw std::runtime_error("overlapping retirement"); }));
	CHECK(retirement.Push("state-one", "state") == Result::Queued);
	CHECK(retirement.Push("state-two", "state") == Result::Coalesced);
	CHECK(retirement.Push("command", {}) == Result::Queued);
	CHECK(retirement.Push("state-three", "state") == Result::Queued);
	CHECK(retirement.Push("excess", {}) == Result::Full);
	CHECK(!retirement.TakeReady()); // Replay must not reach the retiring writer.
	release.set_value();
	retirement.Wait();
	const auto completed = retirement.TakeReady();
	CHECK(completed && completed->overflow && !completed->error);
	CHECK(finished.load() && !retirement.Pending());
	CHECK(completed->replay.size() == 3);
	CHECK(completed->replay[0].value == "state-two");
	CHECK(completed->replay[1].value == "command");
	CHECK(completed->replay[2].value == "state-three");
	CHECK(!retirement.TakeReady()); // Completion is consumed exactly once.

	CHECK(retirement.Start([] {}));
	retirement.Wait();
	const auto next = retirement.TakeReady();
	CHECK(next && next->replay.empty() && !next->overflow && !next->error);
	CHECK(retirement.Start([] { throw std::runtime_error("cleanup failed"); }));
	retirement.Wait();
	const auto failure = retirement.TakeReady();
	CHECK(failure && failure->error && !retirement.Pending());

	// Destruction retains ownership until cleanup finishes; no worker escapes.
	std::promise<void> destructorEntered, destructorRelease;
	const auto allowDestruction = destructorRelease.get_future().share();
	auto owner = std::async(std::launch::async, [&] {
		BrowserHostRetirement local(1);
		local.Start([&] { destructorEntered.set_value(); allowDestruction.wait(); });
	});
	destructorEntered.get_future().wait();
	CHECK(owner.wait_for(0ms) != std::future_status::ready);
	destructorRelease.set_value();
	CHECK(owner.wait_for(1s) == std::future_status::ready);
	owner.get();
	std::cout << "browser_host_retirement_tests: " << g_checks << " checks, " << g_failures << " failures\n";
	return g_failures;
}
