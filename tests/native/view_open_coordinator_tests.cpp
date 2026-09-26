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
		std::unordered_map<std::string, OSFUI::ViewKind> kinds;

		void Add(std::string a_id, OSFUI::ViewKind a_kind = OSFUI::ViewKind::Menu)
		{
			presentation.AddInstantiated({ a_id, a_kind, a_kind == OSFUI::ViewKind::Menu, false, 0 });
			kinds.emplace(a_id, a_kind);
			readiness.emplace(std::move(a_id), Readiness::Loading);
		}
		void Menu(std::string_view a_id)
		{
			if (opens.QueueMenu(a_id, readiness.at(std::string(a_id)), start, start)) {
				presentation.Open(a_id);
			}
		}
		// Mirrors Runtime::ViewOpenReadiness: suspension holds menus, never HUDs.
		std::vector<std::string> CommitReady()
		{
			auto ready = opens.TakeReady([&](std::string_view a_id) {
				const auto it = readiness.find(std::string(a_id));
				if (it == readiness.end()) return Readiness::Missing;
				if (kinds.at(it->first) == OSFUI::ViewKind::Menu && presentation.Suspended()) return Readiness::Suspended;
				return it->second;
			});
			for (const auto& id : ready) presentation.Open(id);
			return ready;
		}
	};
}

int main()
{
	// Opening a cold menu preserves the old menu until load and input are ready.
	// Duplicate opens leave the request pending without changing presentation.
	{
		Fixture f;
		f.Add("mod/old"); f.Add("mod/new");
		f.presentation.Open("mod/old");
		f.Menu("mod/new");
		CHECK(f.CommitReady().empty());
		f.Menu("mod/new");
		f.readiness["mod/new"] = Readiness::WaitingForInput;
		CHECK(f.CommitReady().empty());
		CHECK(f.presentation.ActiveMenu() == "mod/old");
		f.readiness["mod/new"] = Readiness::Ready;
		f.presentation.SetSuspended(true);
		CHECK(f.CommitReady().empty());
		CHECK(f.opens.PendingMenu() == "mod/new"); // held, not dropped
		CHECK(!f.presentation.Open("mod/new")); // suspension refuses menus directly too
		f.presentation.SetSuspended(false);
		CHECK(f.CommitReady() == std::vector<std::string>{ "mod/new" });
		CHECK(f.presentation.ActiveMenu() == "mod/new");
		CHECK(f.CommitReady().empty());
	}

	// A warm menu selects desired presentation immediately and supersedes an old
	// pending request, but leaves unrelated deferred HUDs intact.
	{
		Fixture f;
		f.Add("mod/slow"); f.Add("mod/warm"); f.Add("mod/hud", OSFUI::ViewKind::Hud);
		f.Menu("mod/slow");
		f.opens.QueueHud("mod/hud");
		f.readiness["mod/warm"] = Readiness::Ready;
		f.Menu("mod/warm");
		CHECK(f.presentation.ActiveMenu() == "mod/warm");
		f.readiness["mod/slow"] = Readiness::Ready;
		CHECK(f.CommitReady().empty());
		f.readiness["mod/hud"] = Readiness::Ready;
		CHECK(f.CommitReady() == std::vector<std::string>{ "mod/hud" });
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
		CHECK(f.CommitReady().empty());
		f.readiness["mod/b"] = Readiness::Ready;
		CHECK(f.CommitReady() == std::vector<std::string>{ "mod/b" });
		f.readiness["mod/a"] = Readiness::Loading;
		f.Menu("mod/a");
		CHECK(f.opens.Cancel(*f.opens.PendingMenu()));
		CHECK(f.CommitReady().empty());
	}

	// Warm selection is visible to the next request before browser presentation
	// is committed. Back/close can cancel it without a late load reopening it.
	{
		Fixture f;
		f.Add("mod/old"); f.Add("mod/warm");
		f.presentation.Open("mod/old");
		f.readiness["mod/warm"] = Readiness::Ready;
		f.Menu("mod/warm");
		CHECK(!f.opens.PendingMenu());
		CHECK(f.presentation.ActiveMenu() == "mod/warm");
		CHECK(f.presentation.CloseActiveMenu());
		CHECK(f.CommitReady().empty());
		CHECK(!f.presentation.DesiredVisible());
	}

	// Failed menu loads cancel intent. HUDs survive failure and host recovery,
	// stay behind their load gate, and can resume while menus are prohibited.
	{
		Fixture f;
		f.Add("mod/menu"); f.Add("mod/hud", OSFUI::ViewKind::Hud);
		f.Menu("mod/menu"); f.opens.QueueHud("mod/hud");
		f.opens.OnLoad("mod/menu", true); f.opens.OnLoad("mod/hud", true);
		f.readiness["mod/menu"] = Readiness::Ready;
		CHECK(f.CommitReady().empty());
		f.opens.SuspendMenus();
		f.presentation.SetSuspended(true);
		f.readiness["mod/hud"] = Readiness::Ready;
		CHECK(f.CommitReady() == std::vector<std::string>{ "mod/hud" });
		CHECK(!f.presentation.ActiveMenu());
		CHECK(f.presentation.IsOpen("mod/hud"));
		CHECK(!f.presentation.DesiredVisible());
		f.presentation.SetSuspended(false);
		CHECK(f.presentation.DesiredVisible());
	}

	// Ready HUDs commit without a tick delay. Explicit close and close-all
	// remove deferred work so late loads cannot reopen it.
	{
		Fixture f;
		f.Add("mod/menu"); f.Add("mod/hud", OSFUI::ViewKind::Hud);
		f.readiness["mod/hud"] = Readiness::Ready;
		f.opens.QueueHud("mod/hud");
		CHECK(f.CommitReady() == std::vector<std::string>{ "mod/hud" });
		f.presentation.Close("mod/hud");
		f.opens.QueueHud("mod/hud");
		CHECK(f.opens.Cancel("mod/hud"));
		CHECK(f.CommitReady().empty());
		f.Menu("mod/menu"); f.opens.QueueHud("mod/hud");
		f.opens.Clear(); f.presentation.CloseAll();
		f.readiness["mod/menu"] = Readiness::Ready;
		CHECK(f.CommitReady().empty());
		CHECK(!f.presentation.DesiredVisible());
	}

	// Removed views and failed input integration cancel pending menus rather
	// than retaining a request that might surface after a later recovery.
	for (const auto failure : { Readiness::Missing, Readiness::InputUnavailable }) {
		Fixture f;
		f.Add("mod/menu"); f.Menu("mod/menu");
		f.readiness["mod/menu"] = failure;
		CHECK(f.CommitReady().empty());
		f.readiness["mod/menu"] = Readiness::Ready;
		CHECK(f.CommitReady().empty());
		f.Menu("mod/menu");
		CHECK(f.presentation.IsOpen("mod/menu"));
	}
	{
		Fixture f;
		f.opens.QueueHud("mod/removed");
		CHECK(f.CommitReady().empty());
		CHECK(!f.opens.Contains("mod/removed"));
	}

	// Timing spans enqueue -> instantiation -> load -> first presentable frame,
	// survives queue completion, ignores other views, and is emitted only once.
	{
		ViewOpenCoordinator opens;
		opens.BeginTiming("mod/menu", false, start, start + 10ms);
		opens.BeginTiming("mod/menu", true, start + 15ms, start + 15ms);
		opens.OnInstantiated("mod/menu", start + 30ms);
		CHECK(!opens.QueueMenu("mod/menu", Readiness::Loading, start, start + 40ms));
		opens.OnLoad("mod/other", false, start + 50ms);
		opens.OnLoad("mod/menu", false, start + 90ms);
		CHECK(opens.TakeReady([](auto) { return Readiness::Ready; }).size() == 1);
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
