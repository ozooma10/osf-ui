#include "../../tools/cef_host/LoadState.h"
#include "check.h"
#include <iostream>

int main()
{
    osfui::cefhost::LoadState state;
    CHECK(!state.End("main", true));
    state.Start("main");
    state.Fail("main");
    CHECK(!state.End("main", true)); // Connection error followed by allowed HTTP URL.
    CHECK(!state.End("main", false)); // Preserve the original error, including its code.
    state.Start("main"); // A genuine commit recovers, even when frame/URL are reused.
    CHECK(state.End("main", true) == true); // Includes completed remote HTTP error pages.
    CHECK(!state.End("main", true)); // Duplicate completion cannot publish success again.
    state.Start("old-frame");
    state.Start("replacement-frame");
    CHECK(!state.End("old-frame", true));
    CHECK(state.End("replacement-frame", true) == true);
    state.Fail("main"); // Pre-commit error has no OnLoadStart or matching OnLoadEnd.
    CHECK(!state.End("main", true));
    state.Start("main");
    CHECK(state.End("main", false) == false); // Origin/status rejection still reports failure.
    state.Start("main");
    CHECK(state.End("main", true) == true);
    std::cout << "cef_navigation_tests: " << g_checks << " checks, " << g_failures << " failures\n";
    return g_failures;
}
