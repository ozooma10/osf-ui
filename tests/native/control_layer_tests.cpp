#include "Input/ControlLayer.h"
#include "RE/B/BSInputEnableManager.h"
#include "check.h"

namespace OSFUI::Log { void WarnOnce(std::once_flag&, std::string_view) {} }

int main()
{
    using OSFUI::ControlLayer;
    auto& own=RE::BSInputEnableManager::owned;
    auto& foreign=RE::BSInputEnableManager::foreign;
    foreign.EnableUserEvent(RE::USER_EVENT_FLAG::Movement,false);
    const auto foreignMask=foreign.user;
    ControlLayer::Apply(true);
    CHECK(own.writes==0); // unavailable manager retries on the next runtime update
    RE::BSInputEnableManager::available=true;
    ControlLayer::Apply(true);
    CHECK(own.user!=~0u && own.other!=~0u);
    const auto capturedUser=own.user, capturedOther=own.other;
    ControlLayer::SaveWindow(true);
    CHECK(own.user==~0u && own.other==~0u && foreign.user==foreignMask);
    ControlLayer::Apply(true);
    CHECK(own.user==~0u); // runtime ticks cannot reacquire during serialization
    ControlLayer::SaveWindow(false);
    CHECK(own.user==capturedUser && own.other==capturedOther);
    ControlLayer::SaveWindow(true);
    ControlLayer::Apply(false);
    ControlLayer::SaveWindow(false);
    CHECK(own.user==~0u); // closing during a save cannot be undone by completion
    ControlLayer::Apply(true);
    ControlLayer::BeginRequestedSave();
    ControlLayer::BeginRequestedSave();
    ControlLayer::SaveWindow(true);
    ControlLayer::SaveWindow(false);
    ControlLayer::EndRequestedSave();
    CHECK(own.user==~0u); // nested/overlapping guards retain the outer suspension
    ControlLayer::EndRequestedSave();
    CHECK(own.user==capturedUser);
    ControlLayer::BeginLoad();
    ControlLayer::Apply(true);
    CHECK(own.user==~0u);
    ControlLayer::EndLoad();
    ControlLayer::Apply(false);
    // A prior world's closed panel may still have a mask restored from an old save.
    own.user=capturedUser; own.other=capturedOther;
    ControlLayer::EndLoad();
    CHECK(own.user==~0u && own.other==~0u && foreign.user==foreignMask);
    ControlLayer::Apply(true);
    CHECK(own.user==capturedUser);
    ControlLayer::SaveWindow(true);
    ControlLayer::BeginLoad();
    ControlLayer::SaveWindow(false);
    CHECK(own.user==~0u); // late save completion cannot revive the old world lock
    ControlLayer::EndLoad();
    std::printf("%d checks; %d failures\n",g_checks,g_failures);
    return g_failures;
}
