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
        CHECK(!client.Language());
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
        Install(&settings, &diagnostics);
        OSFSettingsClient client;
        CHECK(client.Initialize());
        CHECK(client.DeveloperMode());
        settings.values["developerMode"] = false;
        CHECK(client.DeveloperMode()); // Startup-only, no live subscription.
        CHECK(settings.reads == 1 && !settings.changed);
    }
    {
        SettingsTest::Settings settings; SettingsTest::Diagnostics diagnostics;
        settings.language = "ptbr";
        Install(&settings, &diagnostics);
        OSFSettingsClient client;
        CHECK(client.Initialize());
        CHECK(settings.languageReads == 0); // Not a startup read; the game language loads later.
        CHECK(client.Language() == "ptbr");
        settings.language = "de";
        CHECK(client.Language() == "ptbr" && settings.languageReads == 1); // Cached once known.
    }
    {
        SettingsTest::Settings settings; SettingsTest::Diagnostics diagnostics;
        settings.languageStatus = Status::NotReady;
        Install(&settings, &diagnostics);
        OSFSettingsClient client;
        CHECK(client.Initialize());
        CHECK(!client.Language());
        CHECK(!client.Language()); // Pending language must not be cached as the default.
        CHECK(settings.languageReads == 2);
        settings.languageStatus = Status::Ok;
        CHECK(client.Language() == "en"); // Retried once the game's translations have loaded.
        CHECK(settings.languageReads == 3);
        CHECK(!diagnostics.Has("settings.language"));
    }
    {
        SettingsTest::Settings settings; SettingsTest::Diagnostics diagnostics;
        Install(&settings, &diagnostics);
        settingsVersion = 0x00010000; // OSF Settings older than ABI 1.1.
        OSFSettingsClient client;
        CHECK(client.Initialize());
        CHECK(client.Language() == "" && settings.languageReads == 0);
    }
    {
        SettingsTest::Settings settings; SettingsTest::Diagnostics diagnostics;
        settings.languageStatus = Status::InternalError;
        Install(&settings, &diagnostics);
        OSFSettingsClient client;
        CHECK(client.Initialize());
        CHECK(!client.Language());
        settings.languageStatus = Status::Ok;
        settings.language = "de";
        CHECK(client.Language() == "de"); // A failed read cannot lock in the browser default.
    }
    {
        SettingsTest::Settings settings; SettingsTest::Diagnostics diagnostics;
        settings.readStatus["developerMode"] = Status::UnknownMod;
        Install(&settings, &diagnostics);
        OSFSettingsClient client;
        CHECK(client.Initialize());
        CHECK(!client.DeveloperMode());
        CHECK(diagnostics.Has("settings.developerMode"));
        CHECK(diagnostics.Get("settings.developerMode").severity == OSFSettings::API::Diagnostics::Severity::Warning);
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
        const std::string id = "view.load-failed:demo/panel";
        const nlohmann::json context{{"view", "demo/panel"}, {"detail", "C:/private/technical.log"}};
        client.ReportFailure(id, "view.load-failed", "ERR_CONNECTION_REFUSED", context);
        CHECK(diagnostics.Has(id));
        // Known codes get fixed user-facing text; technical message and context stay in the log.
        CHECK(diagnostics.Get(id).title == "A mod WebView could not load");
        CHECK(diagnostics.Get(id).impact.find("demo/panel") != std::string::npos);
        CHECK(diagnostics.Get(id).impact.find("private") == std::string::npos);
        client.ReportFailure(id, "view.load-retrying", "ERR_CONNECTION_REFUSED", context);
        CHECK(diagnostics.Get(id).severity == OSFSettings::API::Diagnostics::Severity::Warning);
        // Clearing one issue preserves other issues and other mods' reports.
        diagnostics.Report({.modId = "other", .id = "keep", .title = "Keep me"});
        client.ReportFailure("startup.renderer", "webview.renderer-init", "Browser unavailable");
        client.ClearFailure(id);
        CHECK(!diagnostics.Has(id));
        CHECK(diagnostics.Has("startup.renderer"));
        CHECK(diagnostics.issues.contains({"other", "keep"}));
        client.ClearFailure("startup.renderer");
        CHECK(!diagnostics.Has("startup.renderer"));
        const std::string longId(200, 'x');
        client.ReportFailure(longId, "test", "Long identity");
        CHECK(diagnostics.Has(longId)); // Slim does not impose the former 64-byte limit.
        client.ClearFailure(longId);
        CHECK(!diagnostics.Has(longId));
    }
    return g_failures;
}
