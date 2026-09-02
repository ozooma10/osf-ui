#include "Diagnostics/HealthReconciler.h"
#include "Core/Log.h"

#include <cassert>
#include <iostream>

namespace OSFUI::Log
{
    void WarnOnce(std::once_flag& flag, std::string_view message)
    {
        std::call_once(flag, [&] { REX::test::Log("WARN", std::string(message)); });
    }
    bool DebugEnabled() { return true; }
    void SetDebugLogging(bool) {}
}

int main()
{
    using namespace OSFUI;
    HealthRegistry registry;
    HealthReconciler reconciler;

    reconciler.ReportViewLoad(registry, "acme/view", true, "timeout", -1, 2, 1.0);
    assert(registry.IsActive("view.load-retrying:acme/view"));
    reconciler.ReportViewLoad(registry, "acme/view", true, "timeout", -1, 0, 2.0);
    assert(!registry.IsActive("view.load-retrying:acme/view"));
    assert(registry.IsActive("view.load-failed:acme/view"));
    reconciler.ReportViewLoad(registry, "acme/view", false, "", 0, 0, 3.0);
    assert(!registry.IsActive("view.load-failed:acme/view"));

    reconciler.ReportRendererHealth(registry, "host.disconnected", true, "pipe closed", true, 4.0);
    assert(registry.IsActive("host.disconnected"));
    reconciler.ReportRendererHealth(registry, "host.disconnected", false, "", true, 5.0);
    assert(!registry.IsActive("host.disconnected"));

    reconciler.ReportProtocolMisuse(registry, "acme/view", "unknown-endpoint", 3, 6.0);
    assert(registry.IsActive("view.protocol-misuse:acme/view"));
    std::cout << "runtime_health_tests: ok\n";
}
