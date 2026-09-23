#include "Dependency/OSFSettingsClient.h"
#include "SettingsServices.h"
#include "check.h"

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
    using namespace SettingsTest;
    {
        Install(nullptr, nullptr);
        OSFSettingsClient client;
        CHECK(!client.Initialize());
        CHECK(!client.Available());
        CHECK(!client.AcquireInputSuppression());
    }
    for (int scenario = 0; scenario < 4; ++scenario) {
        SettingsTest::Settings settings; SettingsTest::Diagnostics diagnostics;
        Install(&settings, &diagnostics);
        if (scenario == 0) settingsVersion = 0x00020000;
        if (scenario == 1) diagnosticsVersion = 0x00020000;
        if (scenario == 2) settings.ready = false;
        if (scenario == 3) Install(&settings, nullptr);
        OSFSettingsClient client;
        CHECK(!client.Initialize());
        CHECK(!client.Available());
        CHECK(settings.reads == 0);
    }
    {
        SettingsTest::Settings settings; SettingsTest::Diagnostics diagnostics;
        settings.values["developerMode"] = true;
        settings.values["highRefreshCapture"] = true;
        Install(&settings, &diagnostics);
        OSFSettingsClient client;
        CHECK(client.Initialize());
        CHECK(client.DeveloperMode() && client.HighRefreshCapture());
        settings.values["developerMode"] = false;
        client.SyncDiagnostics({});
        CHECK(client.DeveloperMode()); // Startup-only, no live subscription.
        CHECK(settings.reads == 2 && !settings.changed);
    }
    {
        SettingsTest::Settings settings; SettingsTest::Diagnostics diagnostics;
        settings.readStatus["developerMode"] = Status::UnknownMod;
        settings.readStatus["highRefreshCapture"] = Status::TypeMismatch;
        Install(&settings, &diagnostics);
        OSFSettingsClient client;
        CHECK(client.Initialize());
        CHECK(!client.DeveloperMode() && !client.HighRefreshCapture());
        CHECK(diagnostics.Has("settings.developerMode"));
        CHECK(diagnostics.Get("settings.highRefreshCapture").severity == OSFSettings::API::Diagnostics::Severity::Warning);
    }
    {
        SettingsTest::Settings settings; SettingsTest::Diagnostics diagnostics;
        Install(&settings, &diagnostics);
        OSFSettingsClient client;
        CHECK(client.Initialize());
        CHECK(client.AcquireInputSuppression());
        CHECK(client.AcquireInputSuppression()); // Menu switches keep one token.
        CHECK(settings.acquisitions == 1 && settings.blocks.size() == 1);
        settings.releaseStatus = Status::InternalError;
        CHECK(!client.ReleaseInputSuppression());
        CHECK(settings.blocks.size() == 1);
        settings.releaseStatus = Status::Ok;
        CHECK(client.ReleaseInputSuppression());
        CHECK(client.ReleaseInputSuppression());
        CHECK(settings.releases == 2 && settings.blocks.empty());
        OSFSettings::API::HotkeyBlock otherOwner{};
        CHECK(settings.AcquireHotkeyBlock(&otherOwner) == Status::Ok);
        CHECK(client.AcquireInputSuppression());
        CHECK(client.ReleaseInputSuppression());
        CHECK(settings.blocks == std::set<OSFSettings::API::HotkeyBlock>{otherOwner});
        CHECK(settings.ReleaseHotkeyBlock(otherOwner) == Status::Ok);
        settings.acquireStatus = Status::NotReady;
        CHECK(!client.AcquireInputSuppression());
        CHECK(settings.blocks.empty());
        settings.acquireStatus = Status::Ok;
        CHECK(client.AcquireInputSuppression());
        settings.blocks.clear(); // An already-absent token is not retained forever.
        CHECK(client.ReleaseInputSuppression());
        CHECK(client.AcquireInputSuppression());
        CHECK(client.ReleaseInputSuppression());
    }
    {
        SettingsTest::Settings settings; SettingsTest::Diagnostics diagnostics;
        Install(&settings, &diagnostics);
        OSFSettingsClient client;
        CHECK(client.Initialize());
        HealthRegistry registry;
        HealthRegistry::IssueSpec issue{.id = "view.load-failed:demo/panel", .code = "view.load-failed",
            .severity = HealthRegistry::Severity::Error, .subject = "demo/panel",
            .context = {{"detail", "C:/private/technical.log"}}};
        registry.Upsert(issue);
        diagnostics.reportStatus = Status::InternalError;
        client.SyncDiagnostics(registry.ActiveIssues());
        CHECK(!diagnostics.Has(issue.id));
        diagnostics.reportStatus = Status::Ok;
        client.SyncDiagnostics(registry.ActiveIssues());
        CHECK(diagnostics.Has(issue.id));
        CHECK(diagnostics.Get(issue.id).title == "A mod WebView could not load");
        CHECK(diagnostics.Get(issue.id).impact.find("demo/panel") != std::string::npos);
        CHECK(diagnostics.Get(issue.id).impact.find("private") == std::string::npos);
        const auto count = diagnostics.reports;
        client.SyncDiagnostics(registry.ActiveIssues());
        CHECK(diagnostics.reports == count);
        issue.severity = HealthRegistry::Severity::Warning;
        registry.Upsert(issue);
        diagnostics.reportStatus = Status::InternalError;
        client.SyncDiagnostics(registry.ActiveIssues());
        CHECK(diagnostics.Get(issue.id).severity == OSFSettings::API::Diagnostics::Severity::Error);
        diagnostics.reportStatus = Status::Ok;
        client.SyncDiagnostics(registry.ActiveIssues());
        CHECK(diagnostics.Get(issue.id).severity == OSFSettings::API::Diagnostics::Severity::Warning);
        // A runtime sweep must preserve direct failures and other mods' reports.
        diagnostics.Report({.modId = "other", .id = "keep", .title = "Keep me"});
        client.ReportFailure("startup.renderer", "webview.renderer-init", "Browser unavailable");
        registry.Resolve(issue.id);
        diagnostics.clearStatus = Status::InternalError;
        client.SyncDiagnostics(registry.ActiveIssues());
        CHECK(diagnostics.Has(issue.id));
        diagnostics.clearStatus = Status::Ok;
        client.SyncDiagnostics(registry.ActiveIssues());
        CHECK(!diagnostics.Has(issue.id));
        CHECK(diagnostics.Has("startup.renderer"));
        CHECK(diagnostics.issues.contains({"other", "keep"}));
        diagnostics.clearStatus = Status::InternalError;
        client.ClearFailure("startup.renderer");
        CHECK(diagnostics.Has("startup.renderer"));
        diagnostics.clearStatus = Status::Ok;
        client.SyncDiagnostics({});
        CHECK(!diagnostics.Has("startup.renderer"));
        diagnostics.reportStatus = Status::InternalError;
        registry.Upsert(issue);
        client.SyncDiagnostics(registry.ActiveIssues());
        registry.Resolve(issue.id);
        const auto clearCount = diagnostics.clears;
        client.SyncDiagnostics(registry.ActiveIssues());
        CHECK(diagnostics.clears == clearCount); // Never clear a report that was not accepted.
        diagnostics.reportStatus = Status::Ok;
        const std::string longId(200, 'x');
        client.ReportFailure(longId, "test", "Long identity");
        CHECK(diagnostics.Has(longId)); // Slim does not impose the former 64-byte limit.
        client.ClearFailure(longId);
        CHECK(!diagnostics.Has(longId));
    }
    return g_failures;
}
