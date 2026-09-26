#include "Views/ViewOpenCoordinator.h"
#include "Views/ViewPresentationController.h"
#include "check.h"

#include <unordered_map>

using OSFUI::ViewOpenCoordinator;
using Readiness = ViewOpenCoordinator::Readiness;

namespace
{
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
			if (opens.QueueMenu(a_id, readiness.at(std::string(a_id)))) {
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
		f.opens.OnLoadFailed("mod/menu"); f.opens.OnLoadFailed("mod/hud");
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

	std::printf("view_open_coordinator_tests: %d checks, %d failures\n", g_checks, g_failures);
	return g_failures ? 1 : 0;
}
