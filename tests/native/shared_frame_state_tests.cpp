#include "Render/SharedFrameState.h"
#include "check.h"

namespace
{
	using State = OSFUI::SharedFrameState;
	using Result = State::PublishResult;
	constexpr std::uintptr_t listA = 10, listB = 11, queueA = 20, queueB = 21;

	OSFUI::SharedRingDesc Ring(std::uint64_t a_generation = 1)
	{
		OSFUI::SharedRingDesc ring;
		ring.slotCount = 4;
		ring.width = 1920;
		ring.height = 1080;
		ring.generation = a_generation;
		return ring;
	}
	State Open()
	{
		State state;
		state.BeginRing(Ring());
		state.SetPresentation(1, true);
		state.SetVisible(true);
		return state;
	}
	Result Publish(State& a_state, std::uint32_t a_slot, std::uint64_t a_serial, std::uint64_t a_epoch = 1)
	{
		return a_state.Publish(a_slot, a_serial, 1920, 1080, a_epoch);
	}
	void Submit(State& a_state, std::uintptr_t a_list, std::uintptr_t a_queue, std::uint64_t a_value)
	{
		a_state.Submitted({ &a_list, 1 }, a_queue, a_value);
	}
	void NoReleases(State& a_state)
	{
		CHECK(a_state.TakeReleases() == State::Releases{});
	}
}

int main()
{
	// A failed sibling must not freeze a running scene or falsely complete a menu reveal.
	{
		auto state = Open();
		CHECK(state.Publish(0, 1, 1920, 1080, 1, false) == Result::Accepted);
		CHECK(state.Record(listA, 1, 1)->frameIndex == 1);
		CHECK(!state.Latest());
		state.BeginPreparation();
		CHECK(state.Publish(1, 2, 1920, 1080, 1, false) == Result::Discarded);
		CHECK(state.TakeReleases()[1] == 2);
		CHECK(!state.PreparationReady(1, 2, 1920, 1080));
		CHECK(Publish(state, 1, 3) == Result::Accepted);
		CHECK(state.PreparationReady(1, 3, 1920, 1080));
		CHECK(state.Publish(2, 4, 1920, 1080, 1, false) == Result::Discarded);
		state.EndPreparation(true);
		CHECK(state.Record(listB, 1, 4)->frameIndex == 3);
	}
	// Pin the prepared frame for the launcher handshake, then display the newest
	// capture even if the page stops painting before Settings finishes closing.
	{
		auto state = Open();
		CHECK(Publish(state, 0, 1) == Result::Accepted);
		CHECK(state.Record(listA, 1, 1)->sharedSlot == 0);
		Submit(state, listA, queueA, 1);
		state.Completed(queueA, 1);
		state.BeginPreparation();
		state.SetPresentation(2, true);
		CHECK(!state.PreparationReady(1, 10, 1920, 1080));
		CHECK(Publish(state, 1, 2, 1) == Result::Discarded);
		CHECK(Publish(state, 1, 3, 2) == Result::Accepted);
		CHECK(!state.PreparationReady(1, 2, 1920, 1080));
		CHECK(!state.PreparationReady(2, 3, 1920, 1080));
		CHECK(!state.PreparationReady(1, UINT64_MAX, 1920, 1080));
		CHECK(!state.PreparationReady(1, 3, 2560, 1440));
		CHECK(state.PreparationReady(1, 3, 1920, 1080));
		CHECK(state.Record(listA, 1, 3)->sharedSlot == 0); // Settings still owns the screen
		CHECK(Publish(state, 2, 4, 2) == Result::Accepted);
		CHECK(Publish(state, 2, 5, 2) == Result::Invalid); // following frame still reserved
		CHECK(state.Latest()->frameIndex == 3);
		state.EndPreparation(true);
		CHECK(Publish(state, 3, 5, 2) == Result::Accepted); // coalesce following captures, not the pinned frame
		CHECK(state.TakeReleases()[2] == 4);
		CHECK(state.Record(listB, 1, 2)->sharedSlot == 0); // producer completion is checked again at draw time
		CHECK(state.Record(listB, 1, 3)->sharedSlot == 1);
		CHECK(state.Record(listB, 1, 4)->sharedSlot == 1); // following copy still pending
		CHECK(state.Record(listB, 1, 5)->sharedSlot == 3); // no new Publish call is needed
		CHECK(Publish(state, 2, 6, 2) == Result::Accepted);
		CHECK(state.Record(listB, 1, 5)->sharedSlot == 3);
		CHECK(state.Record(listB, 1, 6)->sharedSlot == 2);
	}
	// Cancel drops the prepared view and its late captures, keeping the old HUD until the restored scene draws.
	{
		auto state = Open();
		CHECK(Publish(state, 0, 1) == Result::Accepted);
		CHECK(state.Record(listA, 1, 1)->sharedSlot == 0);
		state.BeginPreparation();
		state.SetPresentation(2, true);
		CHECK(Publish(state, 1, 2, 2) == Result::Accepted);
		CHECK(Publish(state, 2, 3, 2) == Result::Accepted);
		state.SetPresentation(3, true);
		state.EndPreparation(false);
		CHECK(!state.Latest());
		const auto canceled = state.TakeReleases();
		CHECK(canceled[1] == 2 && canceled[2] == 3);
		CHECK(Publish(state, 1, 4, 2) == Result::Discarded);
		CHECK(state.Record(listB, 1, 3)->sharedSlot == 0);
		CHECK(Publish(state, 1, 5, 3) == Result::Accepted);
		CHECK(state.Record(listB, 1, 5)->sharedSlot == 1);
	}
	// Prepare while hidden; resize or host replacement cancels preparation.
	{
		auto state = Open();
		state.SetVisible(false);
		state.BeginPreparation();
		state.SetPresentation(2, true);
		CHECK(Publish(state, 0, 1, 2) == Result::Accepted);
		CHECK(!state.Record(listA, 1, 1));
		state.BeginRing(Ring(2));
		CHECK(!state.PreparationReady(1, 1, 1920, 1080));
		CHECK(Publish(state, 0, 1, 2) == Result::Accepted);
		CHECK(state.PreparationReady(2, 1, 1920, 1080));
		state.EndPreparation(true);
		state.SetVisible(true);
		CHECK(state.Record(listA, 2, 1)->sharedSlot == 0);
	}
	// Release unused frames immediately.
	{
		auto state = Open();
		CHECK(Publish(state, 0, 1) == Result::Accepted);
		CHECK(Publish(state, 1, 2) == Result::Accepted);
		CHECK(state.TakeReleases()[0] == 1);
		state.SetVisible(false);
		CHECK(state.TakeReleases()[1] == 2);
		CHECK(!state.Latest());
		NoReleases(state);
		state.SetPresentation(2, true);
		CHECK(Publish(state, 2, 3) == Result::Discarded);
		CHECK(state.TakeReleases()[2] == 3);
		CHECK(Publish(state, 2, 4, 2) == Result::Accepted);
		state.SetPresentation(3, true);
		CHECK(state.TakeReleases()[2] == 4);
	}

	// Finishing a draw must not release the displayed slot.
	{
		auto state = Open();
		CHECK(Publish(state, 0, 1) == Result::Accepted);
		CHECK(!state.Record(listA, 1, 0)); // producer not finished
		CHECK(state.Record(listA, 1, 1).has_value());
		Submit(state, listA, queueA, 1);
		state.Completed(queueA, 1);
		CHECK(!state.HasReads());
		NoReleases(state);
		CHECK(state.Record(listB, 1, 1).has_value());
		CHECK(Publish(state, 1, 2) == Result::Accepted);
		CHECK(Publish(state, 2, 3) == Result::Accepted);
		CHECK(state.TakeReleases()[1] == 2);
		CHECK(Publish(state, 3, 4) == Result::Accepted);
		CHECK(state.TakeReleases()[2] == 3);
		CHECK(Publish(state, 0, 5) == Result::Invalid); // Current is still reserved
		CHECK(state.Record(listA, 1, 4)->sharedSlot == 3);
		NoReleases(state); // old Current is retiring, listB is not submitted yet
		Submit(state, listB, queueA, 2);
		state.Completed(queueA, 1);
		NoReleases(state);
		state.Completed(queueA, 2);
		CHECK(state.TakeReleases()[0] == 1);
		CHECK(Publish(state, 0, 5) == Result::Accepted);
	}

	// Keep a frame until both queues finish, even when they finish out of order.
	{
		auto state = Open();
		CHECK(Publish(state, 0, 1) == Result::Accepted);
		CHECK(state.Record(listA, 1, 1).has_value());
		CHECK(state.Record(listA, 1, 1).has_value()); // duplicate in one list
		CHECK(state.Record(listB, 1, 1).has_value());
		state.SetVisible(false);
		CHECK(!state.Record(12, 1, 1));
		Submit(state, listB, queueB, 10);
		state.Completed(queueB, 10);
		NoReleases(state);
		CHECK(state.HasReads());
		Submit(state, listA, queueA, 20);
		state.Completed(queueB, 1000); // another queue's value proves nothing
		state.Completed(queueA, UINT64_MAX); // device removed proves nothing
		NoReleases(state);
		state.Completed(queueA, 20);
		CHECK(state.TakeReleases()[0] == 1);
		CHECK(!state.HasReads());
		NoReleases(state);
	}

	// One command list can reference more than one slot before submission.
	{
		auto state = Open();
		CHECK(Publish(state, 0, 1) == Result::Accepted);
		CHECK(state.Record(listA, 1, 1).has_value());
		CHECK(Publish(state, 1, 2) == Result::Accepted);
		CHECK(state.Record(listA, 1, 2).has_value());
		state.SetVisible(false);
		Submit(state, listA, queueA, 1);
		NoReleases(state);
		state.Completed(queueA, 1);
		const auto released = state.TakeReleases();
		CHECK(released[0] == 1 && released[1] == 2);
	}

	// Replacing the ring or host waits for old GPU reads, which cannot release new slots.
	{
		auto state = Open();
		CHECK(Publish(state, 0, 100) == Result::Accepted);
		CHECK(state.Record(listA, 1, 100).has_value());
		state.Disconnect();
		state.BeginRing(Ring(2));
		CHECK(Publish(state, 0, 1) == Result::Accepted);
		CHECK(state.HasReads());
		CHECK(!state.Record(listB, 1, 100));
		Submit(state, listA, queueA, 5);
		state.Completed(queueA, 5);
		CHECK(!state.HasReads());
		NoReleases(state);
		CHECK(state.Record(listB, 2, 1).has_value());
		state.SetVisible(false);
		Submit(state, listB, 0, 0); // failed queue signal retains the slot
		state.Completed(queueA, 100);
		NoReleases(state);
		CHECK(state.HasReads());
	}

	// All four slots busy: completing one retired slot allows progress without releasing Current or growing the ring.
	{
		auto state = Open();
		for (std::uint32_t slot = 0; slot < 4; ++slot) {
			CHECK(Publish(state, slot, slot + 1) == Result::Accepted);
			CHECK(state.Record(listA + slot, 1, slot + 1).has_value());
			Submit(state, listA + slot, queueA, slot + 1);
		}
		NoReleases(state);
		state.Completed(queueA, 1);
		const auto released = state.TakeReleases();
		CHECK(released[0] == 1 && released[1] == 0 && released[2] == 0 && released[3] == 0);
		CHECK(Publish(state, 0, 5) == Result::Accepted);
		state.Completed(queueA, 4);
		const auto more = state.TakeReleases();
		CHECK(more[0] == 0 && more[1] == 2 && more[2] == 3 && more[3] == 0);
		CHECK(Publish(state, 3, 6) == Result::Invalid); // completed Current still held
	}

	// A busy slot is never reused, even across many close/reopen cycles with delayed submit/completion.
	{
		auto state = Open();
		State::Releases busy{};
		for (std::uint64_t serial = 1; serial <= 48; ++serial) {
			const auto slot = static_cast<std::uint32_t>((serial - 1) % 4);
			CHECK(busy[slot] == 0);
			busy[slot] = serial;
			state.SetVisible(true);
			CHECK(Publish(state, slot, serial) == Result::Accepted);
			if (serial % 3) CHECK(state.Record(listA, 1, serial).has_value());
			state.SetVisible(false);
			if (serial % 3) {
				NoReleases(state);
				Submit(state, listA, queueA, serial);
				state.Completed(queueA, serial - 1);
				NoReleases(state);
				state.Completed(queueA, serial);
			}
			const auto released = state.TakeReleases();
			CHECK(released[slot] == serial);
			busy[slot] = 0;
			CHECK(!state.HasReads());
		}
	}
	// Ignore unrelated command lists in a submission batch.
	{
		auto state = Open();
		constexpr std::uintptr_t unrelated = 99;
		CHECK(Publish(state, 0, 1) == Result::Accepted);
		CHECK(state.Record(listA, 1, 1).has_value());
		CHECK(Publish(state, 1, 2) == Result::Accepted);
		CHECK(state.Record(listB, 1, 2).has_value());
		const std::uintptr_t none[]{ unrelated, 98 };
		CHECK(!state.HasRecorded(none));
		const std::uintptr_t batch[]{ unrelated, listB, listA };
		CHECK(state.HasRecorded(batch));
		state.Submitted(batch, queueA, 7);
		CHECK(!state.HasRecorded());
		state.Completed(queueA, 6);
		CHECK(state.HasReads());
		state.Completed(queueA, 7);
		CHECK(!state.HasReads());
		CHECK(state.TakeReleases()[0] == 1); // superseded Current retired with its read; slot 1 stays Current
	}
	std::printf("shared_frame_state_tests: %d checks, %d failures\n", g_checks, g_failures);
	return g_failures ? 1 : 0;
}
