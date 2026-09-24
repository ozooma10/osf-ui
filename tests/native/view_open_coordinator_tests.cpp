#include "Views/ViewOpenCoordinator.h"
#include "Views/ViewPresentationController.h"
#include "check.h"

#include <unordered_map>

using OSFUI::ViewOpenCoordinator;
using Readiness = ViewOpenCoordinator::Readiness;
using Clock = ViewOpenCoordinator::Clock;
using namespace std::chrono_literals;

namespace
{
	const auto start = Clock::time_point(1s);
	struct Fixture
	{
		ViewOpenCoordinator opens;
		OSFUI::ViewPresentationController presentation;
		std::unordered_map<std::string, Readiness> readiness;

		void Add(std::string a_id, OSFUI::ViewKind a_kind = OSFUI::ViewKind::Menu)
		{
			presentation.AddInstantiated({ a_id, a_kind, a_kind == OSFUI::ViewKind::Menu, false, 0 });
			readiness.emplace(std::move(a_id), Readiness::Loading);
		}
		void Menu(std::string_view a_id, std::uint64_t a_tick = 10, bool a_barrier = false)
		{
			if (opens.QueueMenu(a_id, readiness.at(std::string(a_id)), a_barrier, a_tick, start, start)) {
				presentation.Open(a_id);
			}
		}
		std::vector<std::string> Tick(std::uint64_t a_tick, bool a_host = true, bool a_menus = true)
		{
			auto ready = opens.TakeReady(a_tick, a_host, a_menus, [&](std::string_view a_id) {
				const auto it = readiness.find(std::string(a_id));
				return it == readiness.end() ? Readiness::Missing : it->second;
			});
			for (const auto& id : ready) presentation.Open(id);
			return ready;
		}
	};
}

int main()
{
	// Opening a cold menu preserves the old menu until load, input and the
	// native retained-state barrier have all completed. Duplicate opens don't
	// postpone the barrier; readiness alone cannot bypass it in the same tick.
	{
		Fixture f;
		f.Add("mod/old"); f.Add("mod/new");
		f.presentation.Open("mod/old");
		f.Menu("mod/new", 10, true);
		f.readiness["mod/new"] = Readiness::Ready;
		CHECK(f.Tick(10).empty());
		f.Menu("mod/new", 100, true);
		f.readiness["mod/new"] = Readiness::WaitingForInput;
		CHECK(f.Tick(11).empty());
		CHECK(f.presentation.ActiveMenu() == "mod/old");
		f.readiness["mod/new"] = Readiness::Ready;
		CHECK(f.Tick(11, false).empty());
		CHECK(f.Tick(11, true, false).empty());
		CHECK(f.Tick(11) == std::vector<std::string>{ "mod/new" });
		CHECK(f.presentation.ActiveMenu() == "mod/new");
		CHECK(f.Tick(12).empty());
	}

	// A warm menu without preflight opens immediately and supersedes an old
	// pending request, but leaves unrelated deferred HUDs intact.
	{
		Fixture f;
		f.Add("mod/slow"); f.Add("mod/warm"); f.Add("mod/hud", OSFUI::ViewKind::Hud);
		f.Menu("mod/slow");
		f.opens.QueueHud("mod/hud", 10);
		f.readiness["mod/warm"] = Readiness::Ready;
		f.Menu("mod/warm");
		CHECK(f.presentation.ActiveMenu() == "mod/warm");
		f.readiness["mod/slow"] = Readiness::Ready;
		CHECK(f.Tick(11).empty());
		f.readiness["mod/hud"] = Readiness::Ready;
		CHECK(f.Tick(12) == std::vector<std::string>{ "mod/hud" });
		CHECK(f.presentation.ActiveMenu() == "mod/warm");
	}

	// Back cancels the pending menu without closing the already presented one.
	// A subsequent request can replace another cold request without reviving it.
	{
		Fixture f;
		f.Add("mod/old"); f.Add("mod/a"); f.Add("mod/b");
		f.presentation.Open("mod/old");
		f.Menu("mod/a");
		CHECK(f.opens.CancelMenu());
		CHECK(!f.opens.CancelMenu());
		CHECK(f.presentation.ActiveMenu() == "mod/old");
		f.Menu("mod/a"); f.Menu("mod/b");
		f.readiness["mod/a"] = Readiness::Ready;
		CHECK(f.Tick(11).empty());
		f.readiness["mod/b"] = Readiness::Ready;
		CHECK(f.Tick(12) == std::vector<std::string>{ "mod/b" });
		f.readiness["mod/a"] = Readiness::Loading;
		f.Menu("mod/a");
		CHECK(f.opens.Cancel(*f.opens.PendingMenu()));
		CHECK(f.Tick(13).empty());
	}

	// Failed menu loads cancel intent. HUDs survive failure and host recovery,
	// stay behind their load gate, and can resume while menus are prohibited.
	{
		Fixture f;
		f.Add("mod/menu"); f.Add("mod/hud", OSFUI::ViewKind::Hud);
		f.Menu("mod/menu"); f.opens.QueueHud("mod/hud", 11);
		f.opens.OnLoad("mod/menu", true); f.opens.OnLoad("mod/hud", true);
		f.readiness["mod/menu"] = Readiness::Ready;
		CHECK(f.Tick(11).empty());
		f.opens.SuspendMenus();
		f.presentation.SetSuspended(true);
		f.readiness["mod/hud"] = Readiness::Ready;
		CHECK(f.Tick(12, false).empty());
		CHECK(f.Tick(13, true, false) == std::vector<std::string>{ "mod/hud" });
		CHECK(!f.presentation.ActiveMenu());
		CHECK(f.presentation.IsOpen("mod/hud"));
		CHECK(!f.presentation.DesiredVisible());
		f.presentation.SetSuspended(false);
		CHECK(f.presentation.DesiredVisible());
	}

	// HUD preflight waits a tick even for an already loaded document. Explicit
	// close and close-all remove deferred work so late loads cannot reopen it.
	{
		Fixture f;
		f.Add("mod/menu"); f.Add("mod/hud", OSFUI::ViewKind::Hud);
		f.readiness["mod/hud"] = Readiness::Ready;
		f.opens.QueueHud("mod/hud", 11);
		CHECK(f.Tick(10).empty());
		CHECK(f.opens.Cancel("mod/hud"));
		CHECK(f.Tick(11).empty());
		f.Menu("mod/menu"); f.opens.QueueHud("mod/hud", 12);
		f.opens.Clear(); f.presentation.CloseAll();
		f.readiness["mod/menu"] = Readiness::Ready;
		CHECK(f.Tick(12).empty());
		CHECK(!f.presentation.DesiredVisible());
	}

	// Removed views and failed input integration cancel pending menus rather
	// than retaining a request that might surface after a later recovery.
	for (const auto failure : { Readiness::Missing, Readiness::InputUnavailable }) {
		Fixture f;
		f.Add("mod/menu"); f.Menu("mod/menu");
		f.readiness["mod/menu"] = failure;
		CHECK(f.Tick(11).empty());
		f.readiness["mod/menu"] = Readiness::Ready;
		CHECK(f.Tick(12).empty());
		f.Menu("mod/menu", 12);
		CHECK(f.presentation.IsOpen("mod/menu"));
	}
	{
		Fixture f;
		f.opens.QueueHud("mod/removed", 10);
		CHECK(f.Tick(10).empty());
		CHECK(!f.opens.Contains("mod/removed"));
	}

	// Timing spans enqueue -> instantiation -> load -> first presentable frame,
	// survives queue completion, ignores other views, and is emitted only once.
	{
		ViewOpenCoordinator opens;
		opens.BeginTiming("mod/menu", false, start, start + 10ms);
		opens.BeginTiming("mod/menu", true, start + 15ms, start + 15ms);
		opens.OnInstantiated("mod/menu", start + 30ms);
		CHECK(!opens.QueueMenu("mod/menu", Readiness::Loading, true, 10, start, start + 40ms));
		opens.OnLoad("mod/other", false, start + 50ms);
		opens.OnLoad("mod/menu", false, start + 90ms);
		CHECK(opens.TakeReady(11, true, true, [](auto) { return Readiness::Ready; }).size() == 1);
		CHECK(!opens.FinishTiming("mod/other", start + 100ms));
		const auto timing = opens.FinishTiming("mod/menu", start + 110ms);
		CHECK(timing.has_value());
		if (timing) {
			CHECK(timing->view == "mod/menu");
			CHECK(timing->totalMs == 110);
			CHECK(timing->instantiateMs == 30);
			CHECK(timing->loadMs == 60);
			CHECK(timing->presentMs == 20);
		}
		CHECK(!opens.FinishTiming("mod/menu", start + 120ms));
	}

	// All cancellation paths retire timing too, even after a menu has left the
	// pending queue. A failed load cannot leave timing attached to a later open.
	for (int cancel = 0; cancel < 4; ++cancel) {
		ViewOpenCoordinator opens;
		opens.BeginTiming("mod/menu", true, start, start);
		opens.OnLoad("mod/menu", false, start + 10ms);
		if (cancel == 0) opens.Cancel("mod/menu");
		if (cancel == 1) opens.SuspendMenus();
		if (cancel == 2) opens.Clear();
		if (cancel == 3) opens.OnLoad("mod/menu", true);
		CHECK(!opens.FinishTiming("mod/menu", start + 20ms));
	}
	for (const auto requested : { Clock::time_point{}, start + 1s }) {
		ViewOpenCoordinator opens;
		opens.BeginTiming("mod/menu", true, requested, start);
		opens.OnLoad("mod/menu", false, start + 10ms);
		const auto timing = opens.FinishTiming("mod/menu", start + 20ms);
		CHECK(timing && timing->totalMs == 20);
	}

	std::printf("view_open_coordinator_tests: %d checks, %d failures\n", g_checks, g_failures);
	return g_failures ? 1 : 0;
}
