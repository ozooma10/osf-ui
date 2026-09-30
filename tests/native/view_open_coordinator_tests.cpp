#include "Views/ViewOpenCoordinator.h"
#include "Views/ViewPresentationController.h"
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

    // Preparation reaches frame readiness without changing input ownership.
    OSFUI::ViewPresentationController presentation;
    presentation.AddInstantiated({ .id = "mod/menu", .kind = OSFUI::ViewKind::Menu, .capturesInput = true, .pausesGame = true });
    for (const auto request : { std::uint64_t{0}, std::uint64_t{41} }) {
        opens.QueueMenu("mod/menu", 130.0, request);
        CHECK(opens.Contains("mod/menu"));
        CHECK(!opens.TakeReadyMenu("mod/menu", request));
        for (const auto held : { Readiness::Loading, Readiness::WaitingForInput, Readiness::Suspended }) {
            readiness["mod/menu"] = held;
            CHECK(ready().empty());
            CHECK(opens.PendingMenu()->phase == Phase::Loading);
        }
        readiness["mod/menu"] = Readiness::Ready;
        CHECK(ready() == std::vector<std::string>{"mod/menu"});
        CHECK(opens.PendingMenu()->phase == Phase::Rendering);
        CHECK(opens.PendingMenu()->requestId == request);
        CHECK(!opens.TakeReadyMenu("mod/menu", request)); // page readiness is not a prepared frame
        CHECK(ready().empty());
        opens.PendingMenu()->phase = Phase::Ready; // renderer confirms GPU-ready retained frame
        CHECK(ready().empty());
        CHECK(!presentation.DesiredCapture());
        CHECK(!presentation.DesiredPause());
        CHECK(!opens.TakeReadyMenu("mod/other"));
        CHECK(opens.Contains("mod/menu"));
        CHECK(!opens.TakeReadyMenu("mod/menu", request + 1));
        // Only this request's after-close callback can consume its frame, once.
        const auto finished = opens.TakeReadyMenu("mod/menu", request);
        CHECK(finished && finished->requestId == request);
        CHECK(presentation.Open(finished->view));
        CHECK(presentation.DesiredCapture());
        CHECK(presentation.DesiredPause());
        CHECK(!opens.PendingMenu());
        CHECK(!opens.TakeReadyMenu("mod/menu", request));
        CHECK(!opens.TakeMenu());
        presentation.CloseActiveMenu();
    }

    // Runtime cleanup may discard any preparation stage, including an accepted
    // completion whose after-close callback never arrived.
    for (const auto phase : { Phase::Loading, Phase::Rendering, Phase::Ready }) {
        opens.QueueMenu("mod/menu", 210.0, 46);
        opens.PendingMenu()->phase = phase;
        const auto canceled = opens.TakeMenu();
        CHECK(canceled && canceled->requestId == 46);
        CHECK(!opens.TakeReadyMenu("mod/menu", 46));
        CHECK(ready().empty());
    }

    // An old callback cannot activate a reopened request for the same view.
    opens.QueueMenu("mod/menu", 220.0, 47);
    opens.PendingMenu()->phase = Phase::Ready;
    CHECK(!opens.TakeReadyMenu("mod/menu", 46));
    CHECK(!opens.TakeReadyMenu("mod/menu"));
    CHECK(opens.TakeReadyMenu("mod/menu", 47).has_value());

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

    std::printf("view_open_coordinator_tests: %d checks, %d failures\n", g_checks, g_failures);
    return g_failures ? 1 : 0;
}
