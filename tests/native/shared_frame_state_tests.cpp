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
	// Superseded/hidden/pre-reveal frames were never recorded and release now.
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

	// A completed draw does not release Current. Repeated draws and pending
	// replacement churn must never let the host reuse the displayed slot.
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

	// All reads matter: hide while two lists reference the same frame, submit
	// on separate queues, and complete them out of order.
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

	// Ring/host replacement invalidates selection, but old recorded work still
	// blocks GPU resource adoption. Its eventual completion cannot ack new slots.
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

	// Saturate all four slots with delayed GPU reads. Completing a single
	// retired slot permits progress without releasing Current or growing ring.
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

	// Four slots support bounded progress; a busy slot is never reused, even
	// across many close/reopen cycles with delayed submission/completion.
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
	std::printf("shared_frame_state_tests: %d checks, %d failures\n", g_checks, g_failures);
	return g_failures ? 1 : 0;
}
