#include "Views/ViewOpenCoordinator.h"
#include "check.h"

#include <unordered_map>

using OSFUI::ViewOpenCoordinator;
using Readiness = ViewOpenCoordinator::Readiness;
using Phase = ViewOpenCoordinator::Phase;

int main()
{
    ViewOpenCoordinator opens;
    std::unordered_map<std::string, Readiness> readiness;
    const auto ready = [&] {
        return opens.TakeReady([&](std::string_view id) {
            const auto it = readiness.find(std::string(id));
            return it == readiness.end() ? Readiness::Missing : it->second;
        });
    };

    // Both ordinary and launcher opens stay pending after page readiness.
    // That notification starts rendering; only Runtime's submission observation
    // may finish the operation and select the new menu/input owner.
    for (const auto request : { std::uint64_t{0}, std::uint64_t{41} }) {
        opens.QueueMenu("mod/menu", 130.0, request);
        CHECK(opens.Contains("mod/menu"));
        for (const auto held : { Readiness::Loading, Readiness::WaitingForInput, Readiness::Suspended }) {
            readiness["mod/menu"] = held;
            CHECK(ready().empty());
            CHECK(opens.PendingMenu()->phase == Phase::Loading);
        }
        readiness["mod/menu"] = Readiness::Ready;
        CHECK(ready() == std::vector<std::string>{"mod/menu"});
        CHECK(opens.PendingMenu()->phase == Phase::Rendering);
        CHECK(opens.PendingMenu()->requestId == request);
        CHECK(opens.PendingMenu()->deadline == 130.0);
        CHECK(ready().empty()); // no duplicate start while rendering
        opens.PendingMenu()->phase = Phase::AwaitingSubmission;
        CHECK(ready().empty());
        CHECK(opens.Contains("mod/menu"));
        const auto finished = opens.TakeMenu();
        CHECK(finished && finished->view == "mod/menu" && finished->requestId == request);
        CHECK(finished->phase == Phase::AwaitingSubmission);
        CHECK(!opens.PendingMenu() && !opens.Contains("mod/menu"));
        CHECK(!opens.TakeMenu()); // completion/cleanup owns the operation once
    }

    // Failure leaves the completion identity available for Runtime's common
    // cleanup, rather than silently dropping a Settings waiter.
    for (const auto failure : { Readiness::Missing, Readiness::InputUnavailable }) {
        opens.QueueMenu("mod/menu", 230.0, 42);
        readiness["mod/menu"] = failure;
        CHECK(ready().empty());
        const auto failed = opens.TakeMenu();
        CHECK(failed && failed->requestId == 42 && failed->phase == Phase::Loading);
        readiness["mod/menu"] = Readiness::Ready;
        CHECK(ready().empty()); // late readiness cannot revive a canceled open
    }

    // Replacing/canceling a menu preserves unrelated HUD demand. Ready HUDs
    // still complete at load readiness, including while a menu awaits a frame.
    opens.QueueHud("mod/hud");
    readiness["mod/hud"] = Readiness::Loading;
    opens.QueueMenu("mod/old", 300.0, 43);
    const auto replaced = opens.TakeMenu();
    CHECK(replaced && replaced->requestId == 43);
    opens.QueueMenu("mod/new", 310.0, 44);
    readiness["mod/old"] = Readiness::Ready;
    readiness["mod/new"] = Readiness::Loading;
    CHECK(ready().empty());
    readiness["mod/hud"] = Readiness::Ready;
    CHECK(ready() == std::vector<std::string>{"mod/hud"});
    CHECK(opens.PendingMenu()->view == "mod/new");
    CHECK(opens.PendingMenu()->requestId == 44);
    CHECK(opens.TakeMenu()->requestId == 44);
    readiness["mod/new"] = Readiness::Ready;
    CHECK(ready().empty());

    opens.QueueHud("mod/hud");
    CHECK(opens.CancelHud("mod/hud"));
    CHECK(!opens.CancelHud("mod/hud"));
    CHECK(ready().empty());
    opens.QueueHud("mod/removed");
    CHECK(ready().empty());
    CHECK(!opens.Contains("mod/removed"));
    opens.QueueHud("mod/hud");
    opens.ClearHuds();
    CHECK(ready().empty());

    std::printf("view_open_coordinator_tests: %d checks, %d failures\n", g_checks, g_failures);
    return g_failures ? 1 : 0;
}
