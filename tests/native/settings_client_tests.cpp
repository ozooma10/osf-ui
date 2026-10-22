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
        CHECK(!client.AllowWebBrowsing());
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
        settings.values["cefPrototype"] = true;
        Install(&settings, &diagnostics);
        OSFSettingsClient client;
        CHECK(client.Initialize());
        CHECK(client.DeveloperMode());
        settings.values["developerMode"] = false;
        settings.values["cefPrototype"] = false;
        CHECK(client.DeveloperMode()); // Startup-only, no live subscription.
        CHECK(settings.reads == 2 && !settings.changed);
        CHECK(!client.AllowWebBrowsing()); // Developer mode is not consent.
    }
    for (const bool enabled : {false,true}) {
        SettingsTest::Settings settings; SettingsTest::Diagnostics diagnostics;
        settings.values["allowWebBrowsing"] = enabled;
        Install(&settings,&diagnostics);
        OSFSettingsClient client;
        CHECK(!client.AllowWebBrowsing());
        CHECK(client.Initialize());
        CHECK(client.AllowWebBrowsing() == enabled && !client.DeveloperMode());
        // Consent is declared by the World Screens extension, not by core's own schema.
        CHECK(settings.readKeys.at(0).first == "osfui" && settings.readKeys.at(0).second == "developerMode");
        CHECK(settings.readKeys.at(1).first == "osfuiworldscreens");
        CHECK(settings.readKeys.at(1).second == "allowWebBrowsing");
        settings.values["allowWebBrowsing"] = !enabled;
        CHECK(client.AllowWebBrowsing() == enabled); // A running host keeps its launch-time trust policy.
        OSFSettingsClient nextLaunch;
        CHECK(nextLaunch.Initialize());
        CHECK(nextLaunch.AllowWebBrowsing() == !enabled);
    }
    {
        SettingsTest::Settings settings; SettingsTest::Diagnostics diagnostics;
        settings.values["allowWebBrowsing"] = true;
        settings.missingMods.insert("osfuiworldscreens"); // The extension is optional and simply absent.
        Install(&settings,&diagnostics);
        OSFSettingsClient client;
        CHECK(client.Initialize() && !client.AllowWebBrowsing());
        CHECK(!diagnostics.Has("settings.allowWebBrowsing")); // Not installed is not a broken installation.
    }
    for (const auto status : {Status::UnknownSetting,Status::TypeMismatch,Status::NotReady,Status::InternalError}) {
        SettingsTest::Settings settings; SettingsTest::Diagnostics diagnostics;
        settings.values["allowWebBrowsing"] = true;
        settings.readStatus["allowWebBrowsing"] = status;
        Install(&settings,&diagnostics);
        OSFSettingsClient client;
        CHECK(client.Initialize()); // Only browsing is disabled; ordinary OSF UI remains usable.
        CHECK(!client.AllowWebBrowsing());
        CHECK(diagnostics.Has("settings.allowWebBrowsing"));
    }
    {
        SettingsTest::Settings settings; SettingsTest::Diagnostics diagnostics;
        settings.values.erase("allowWebBrowsing"); // An older/missing schema cannot grant consent.
        Install(&settings,&diagnostics);
        OSFSettingsClient client;
        CHECK(client.Initialize() && !client.AllowWebBrowsing());
        CHECK(diagnostics.Has("settings.allowWebBrowsing"));
    }
    {
        const auto root=std::filesystem::path(__FILE__).parent_path().parent_path().parent_path();
        const auto consentSettings=[](const std::filesystem::path& path) {
            std::ifstream file(path);
            const auto schema=nlohmann::json::parse(file);
            int found=0;
            for (const auto& group : schema.at("groups")) for (const auto& setting : group)
                if (setting.at("key") == "allowWebBrowsing") {
                    ++found;
                    CHECK(setting.at("type") == "bool" && setting.at("default").is_boolean() && setting.at("default") == false);
                    CHECK(setting.at("requires") == "restart");
                }
            return found;
        };
        // The schema filename is the settings mod ID core reads consent from.
        CHECK(consentSettings(root / "extensions/world-screens/data/SFSE/Plugins/OSF/Settings/schemas/osfuiworldscreens.json") == 1);
        // Core must not offer the setting to players without the extension.
        CHECK(consentSettings(root / "data/SFSE/Plugins/OSF/Settings/schemas/osfui.json") == 0);
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
        settingsVersion = 0x00010000; // GetLanguage is part of the initial ABI 1.0 contract.
        settings.language = "ja";
        OSFSettingsClient client;
        CHECK(client.Initialize());
        CHECK(client.Language() == "ja" && settings.languageReads == 1);
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
        CHECK(diagnostics.Has(longId));
        client.ClearFailure(longId);
        CHECK(!diagnostics.Has(longId));
    }
    {
        OSFUI::OSFSettingsClient client;
        SettingsTest::Settings rendererSettings;
        SettingsTest::Diagnostics rendererDiagnostics;
        Install(&rendererSettings, &rendererDiagnostics);
        const auto& diagnostics = rendererDiagnostics;
        CHECK(client.Initialize());
        client.ReportFailure("runtime.renderer", "webview.renderer-failed", "Browser stopped",
            {{"stage", "capture-ring"}, {"detail", "texture CreateSharedHandle failed"},
             {"errorCode", 0x8007000Eu}, {"retrying", true}});
        CHECK(diagnostics.Get("runtime.renderer").title.find("CreateSharedHandle") != std::string::npos);
        CHECK(diagnostics.Get("runtime.renderer").title.find("8007000E") != std::string::npos);
        CHECK(diagnostics.Get("runtime.renderer").nextSteps.find("restarting") != std::string::npos);
        client.ReportFailure("runtime.renderer", "webview.renderer-failed", "Browser stopped",
            {{"detail", "Capture failed"}, {"exhausted", true}});
        CHECK(diagnostics.Get("runtime.renderer").nextSteps.find("three attempts") != std::string::npos);
        CHECK(diagnostics.Get("runtime.renderer").nextSteps.find("Open a view to retry") != std::string::npos);
        client.ReportFailure("runtime.renderer", "webview.renderer-failed", "Browser stopped",
            {{"stage", "environment"}, {"errorCode", 0x80070002u}});
        CHECK(diagnostics.Get("runtime.renderer").title == "Microsoft Edge WebView2 Runtime is missing");
        CHECK(diagnostics.Get("runtime.renderer").nextSteps.find("Install") != std::string::npos);
        client.ReportFailure("runtime.renderer", "webview.renderer-failed", "Browser stopped",
            {{"stage", "game-device"}, {"detail", "Starfield's graphics device is unavailable"}});
        CHECK(diagnostics.Get("runtime.renderer").nextSteps.find("Restart Starfield") != std::string::npos);
        client.ClearFailure("runtime.renderer");
        CHECK(!diagnostics.Has("runtime.renderer"));
    }
    return g_failures;
}
