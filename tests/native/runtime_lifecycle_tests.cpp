#include "Runtime/Runtime.h"
#include "RuntimeDoubles.h"
#include "BrowserMessages.h"
#include "check.h"

#include <thread>

using namespace OSFUI;
using namespace RuntimeTest;

namespace
{
    struct FixtureFiles
    {
        FixtureFiles()
        {
            const auto suffix = std::chrono::steady_clock::now().time_since_epoch().count();
            env.dataDir = std::filesystem::temp_directory_path() / ("osfui-lifecycle-" + std::to_string(suffix));
            if (!std::filesystem::create_directory(env.dataDir)) throw std::runtime_error("fixture directory already exists");
            // Exercise the WebView2 default without backend.json; CEF writes its selection below.
            Write("acme/menu", R"({"manifestVersion":1,"kind":"menu"})");
            Write("acme/second", R"({"manifestVersion":1,"kind":"menu"})");
            Write("acme/hud", R"({"manifestVersion":1,"kind":"hud","openOnStart":true})");
            Write("other/panel", R"({"manifestVersion":1,"kind":"menu"})");
            Write("osfui/settings", R"({"manifestVersion":1})");
            Write("osfui/keybinds", R"({"manifestVersion":1})");
        }
        ~FixtureFiles()
        {
            std::error_code ec;
            std::filesystem::remove_all(env.dataDir, ec); // Owns the exclusively created temporary directory.
        }
        static void Write(std::string_view view, std::string_view json)
        {
            const auto directory = env.dataDir / "views" / view;
            std::filesystem::create_directories(directory);
            std::ofstream(directory / "manifest.json") << json;
        }
    };

    bool expectedCef = false;

    Runtime& runtime() { return Runtime::Get(); }
    API::BridgeApi& api() { return API::BridgeApi::Get(); }
    std::vector<std::pair<std::string, API::ViewLifecyclePhase>> lifecycle;
    std::vector<API::RelativePointerPhase> pointer;

    void OnLifecycle(const char* view, API::ViewLifecyclePhase phase, void*) noexcept
    {
        lifecycle.emplace_back(view, phase);
        if (phase == API::ViewLifecyclePhase::kShown) {
            CHECK(runtime().IsInputCaptured());
            CHECK(env.paused && env.controlsBlocked && env.cursorFree);
            CHECK(!env.settings.blocks.empty());
        } else if (phase == API::ViewLifecyclePhase::kHidden && !runtime().IsInputCaptured()) {
            // Observers must see settled engine policy when the last menu closes.
            CHECK(!env.paused && !env.controlsBlocked && !env.cursorFree && !env.browserFocus);
        }
    }
    void OnPointer(const char*, API::RelativePointerPhase phase, float, float, float, void*) noexcept
    {
        pointer.push_back(phase);
    }

    int CountLifecycle(std::string_view view, API::ViewLifecyclePhase phase)
    {
        return static_cast<int>(std::ranges::count_if(lifecycle, [&](const auto& item) {
            return item.first == view && item.second == phase;
        }));
    }

    std::optional<nlohmann::json> StateFor(std::string_view view, std::string_view key)
    {
        std::optional<nlohmann::json> value;
        for (const auto& message : env.sent) {
            const auto& body = message.body;
            if (message.view == view && body.value("kind", "") == "state" && body.value("key", "") == key) {
                value = body.at("value");
            }
        }
        return value;
    }

    void CheckReleased()
    {
        CHECK(!runtime().IsInputCaptured());
        CHECK(!env.browserFocus && !env.controlsBlocked && !env.paused && !env.cursorFree);
        CHECK(env.settings.blocks.empty());
    }

    void PluginLoad()
    {
        CHECK(runtime().Initialize());
        CHECK(env.settings.reads == 0);
        CHECK(env.rendererInitializations == 0 && env.compositorCreations == 0);
        CHECK(env.papyrusInstalls == 0 && env.inputHookInstallations == 0);
        CHECK(api().RegisterView("acme/menu")); // Catalog exists before peers are acquired.
        CHECK(!api().RegisterView("osfui/settings"));
        CHECK(!api().RegisterView("OSFUI/KEYBINDS"));
        CHECK(!api().RequestMenu("osfui/settings", true));
        CHECK(!api().RequestMenu("osfui/keybinds", true));
        CHECK(api().RegisterViewLifecycle("acme/menu", &OnLifecycle, nullptr));
        CHECK(api().RegisterViewLifecycle("acme/second", &OnLifecycle, nullptr));
        CHECK(api().RegisterRelativePointer("acme/menu", &OnPointer, nullptr));
        CHECK(!runtime().OnGameWindowKeyboard({ .vk = 0x79, .down = true })); // F10 before capture is untouched.
    }

    void PostLoad()
    {
        runtime().OnPostLoad();
        CHECK(env.rendererInitializations == 1 && env.compositorCreations == 1);
        CHECK(env.rendererUsesCef == expectedCef);
        CHECK(env.navigations.empty()); // Preparing services does not navigate/start a host.
        CHECK(env.papyrusInstalls == 0 && env.inputHookInstallations == 0);
    }

    void DataReady()
    {
        runtime().OnPostPostDataLoad();
        CHECK(env.papyrusInstalls == 0 && env.inputHookInstallations == 0);
        runtime().Update();
        CHECK(env.filePreparations == 1 && env.papyrusInstalls == 1);
        CHECK(env.menuEventInstalls == 1 && env.focusRegistrations == 1 && env.inputHookInstallations == 1);
        runtime().Update();
        CHECK(env.papyrusInstalls == 1 && env.inputHookInstallations == 1); // Pending work consumed once.
    }

    void LoadHud()
    {
        CHECK(std::ranges::count(env.navigations, "acme/hud") == 1);
        CHECK(env.hidden.at("acme/hud")); // Autostart waits for its own load response.
        QueueLoad("acme/hud");
        QueueHello("acme/hud");
        runtime().Update();
        CHECK(!env.hidden.at("acme/hud"));
        CheckReleased();
    }

    void OpenMenu(std::string_view view = "acme/menu")
    {
        const auto shownBefore = CountLifecycle(view, API::ViewLifecyclePhase::kShown);
        const bool capturedBefore = runtime().IsInputCaptured();
        env.preparedFrameReady = false;
        runtime().EnqueueOpenView(std::string(view));
        runtime().Update();
        QueueLoad(std::string(view));
        QueueHello(std::string(view));
        runtime().Update();
        CHECK(CountLifecycle(view, API::ViewLifecyclePhase::kShown) == shownBefore);
        CHECK(runtime().IsInputCaptured() == capturedBefore);
        env.preparedFrameReady = true;
        runtime().Update();
        CHECK(runtime().IsInputCaptured());
        CHECK(env.inputTarget == view && !env.hidden.at(std::string(view)));
        CHECK(env.paused && env.controlsBlocked && env.cursorFree);
        CHECK(env.settings.blocks.size() == 1);
        CHECK(CountLifecycle(view, API::ViewLifecyclePhase::kShown) == shownBefore + 1);
    }

    void NormalLifecycle()
    {
        PluginLoad();
        CHECK(api().SetState("acme", "native", R"({"revision":1})"));
        CHECK(api().SetState("acme", "native", R"({"revision":2})"));
        env.papyrus.states.push_back({ "acme", "script", { { "formId", 42 } } });
        PostLoad();
        // A menu can load before engine readiness, but cannot capture input yet.
        runtime().EnqueueOpenView("acme/menu");
        runtime().Update();
        QueueLoad("acme/menu");
        QueueHello("acme/menu");
        runtime().Update();
        CheckReleased();
        CHECK(env.inputTarget.empty());
        CHECK(StateFor("acme/menu", "native") == nlohmann::json({ { "revision", 2 } }));
        CHECK(StateFor("acme/menu", "script") == nlohmann::json({ { "formId", 42 } }));
        DataReady();
        LoadHud();
        CHECK(StateFor("acme/hud", "native") == nlohmann::json({ { "revision", 2 } }));

        OpenMenu();
        CHECK(!env.browserFocus); // Showing the sentinel is only a queued engine request.
        CHECK(env.focusOpenRequests == 1);
        env.engineFocus = true;
        runtime().Update();
        CHECK(env.browserFocus);
        CHECK(runtime().OnGameWindowKeyboard({ .vk = 0x79, .down = true }));
        CHECK(env.keys == std::vector<std::uint32_t>{ 0x79 }); // F10 is ordinary page input.
        CHECK(CountLifecycle("acme/menu", API::ViewLifecyclePhase::kShown) == 1);

        // Alt-Tab loss arrives, but another window hook swallows the focus-gain message.
        env.gameWindowActive = false;
        runtime().OnGameWindowActivation(false);
        runtime().Update();
        CHECK(!env.rendererWindowActive);
        CHECK(runtime().IsVisible() && runtime().IsInputCaptured() && env.controlsBlocked);
        env.gameWindowActive = true;
        runtime().EnqueuePresentationRequest(ViewPresentationRequest::Back);
        runtime().Update();
        CHECK(env.rendererWindowActive);
        CHECK(env.backWindowActive == std::vector<bool>{ true }); // Repair before routing Escape.
        CHECK(env.browserFocus && runtime().IsInputCaptured() && env.controlsBlocked);

        // Foreground sampling also revokes browser input when no loss message arrives.
        env.gameWindowActive = false;
        runtime().Update();
        CHECK(!env.rendererWindowActive);
        env.gameWindowActive = true;
        runtime().Update();
        CHECK(env.rendererWindowActive);

        OpenMenu("acme/second");
        CHECK(CountLifecycle("acme/menu", API::ViewLifecyclePhase::kHidden) == 1);
        CHECK(env.settings.acquisitions == 1); // Menu switch retains a single suppression lease.
        runtime().EnqueueCloseView("acme/second");
        runtime().Update();
        CheckReleased();
        CHECK(env.focusCloseRequests == 1);
        CHECK(CountLifecycle("acme/second", API::ViewLifecyclePhase::kHidden) == 1);
        runtime().Update();
        CHECK(env.settings.releases == 1);
        CHECK(CountLifecycle("acme/second", API::ViewLifecyclePhase::kHidden) == 1);

        // Game load drops session-scoped state, preserves native state, and keeps mod ownership.
        env.papyrus.sessionReset = true;
        env.papyrus.states.push_back({ "acme", "new-script", 7 });
        runtime().Update();
        env.sent.clear();
        QueueHello("acme/menu");
        runtime().Update();
        CHECK(!StateFor("acme/menu", "script"));
        CHECK(StateFor("acme/menu", "native") == nlohmann::json({ { "revision", 2 } }));
        CHECK(StateFor("acme/menu", "new-script") == 7);
        runtime().EnqueueOpenView("other/panel");
        runtime().Update();
        QueueHello("other/panel");
        runtime().Update();
        CHECK(!StateFor("other/panel", "native") && !StateFor("other/panel", "new-script"));
        runtime().EnqueueCloseView("other/panel");
        runtime().Update();

        env.transition = true;
        runtime().Update();
        CHECK(!runtime().IsVisible() && env.hidden.at("acme/hud"));
        CheckReleased();
        env.transition = false;
        runtime().Update();
        CHECK(!env.hidden.at("acme/hud"));
        runtime().EnqueueCloseView("acme/hud");
        runtime().Update();
        env.transition = true;
        runtime().Update();
        env.transition = false;
        runtime().Update();
        CHECK(!runtime().IsVisible() && env.hidden.at("acme/hud")); // Explicit close survives suspension.
    }

    void StartupFailure(std::string_view scenario)
    {
        PluginLoad();
        if (scenario == "missing-settings") env.settings.ready = false;
        else env.rendererInitializes = false;
        runtime().OnPostLoad();
        CHECK(env.compositorCreations == 0 && env.navigations.empty());
        CHECK(env.diagnostics.Has(scenario == "missing-settings" ? "dependency" : "startup.renderer"));
        runtime().EnqueueOpenView("acme/menu");
        runtime().OnPostPostDataLoad();
        runtime().Update();
        CheckReleased();
        CHECK(env.navigations.empty());
        if (scenario == "missing-settings") {
            CHECK(env.rendererInitializations == 0 && env.inputHookInstallations == 0);
        } else {
            CHECK(env.rendererInitializations == 1);
        }
    }

    void InputFailure(std::string_view scenario)
    {
        PluginLoad();
        PostLoad();
        if (scenario == "input-hook-failure") env.inputHookInstalls = false;
        if (scenario == "focus-menu-failure") env.focusMenuRegisters = false;
        if (scenario == "menu-events-failure") env.menuEventsInstall = false;
        DataReady();
        CHECK(env.diagnostics.Has("startup.input"));
        runtime().EnqueueOpenView("acme/menu");
        runtime().Update();
        CheckReleased();
        CHECK(std::ranges::count(env.navigations, "acme/menu") == 0);
        CHECK(env.inputTarget.empty());
        CHECK(env.diagnostics.Has("view.acme/menu"));
    }

    void SuppressionFailure()
    {
        PluginLoad();
        PostLoad();
        DataReady();
        env.settings.acquireStatus = OSFSettings::API::Status::InternalError;
        runtime().EnqueueOpenView("acme/menu");
        runtime().Update();
        QueueLoad("acme/menu");
        runtime().Update();
        env.preparedFrameReady = true;
        runtime().Update();
        CheckReleased();
        CHECK(env.inputTarget.empty());
        CHECK(CountLifecycle("acme/menu", API::ViewLifecyclePhase::kShown) == 0);
        CHECK(env.diagnostics.Has("input.hotkey-block"));

        env.settings.acquireStatus = OSFSettings::API::Status::Ok;
        OpenMenu();
        env.settings.releaseStatus = OSFSettings::API::Status::InternalError;
        runtime().EnqueueCloseView("acme/menu");
        runtime().Update();
        CHECK(!runtime().IsInputCaptured());
        CHECK(!env.paused && !env.cursorFree && !env.controlsBlocked);
        CHECK(env.settings.blocks.size() == 1); // Failed lease release remains owned and retryable.
        const auto releases = env.settings.releases;
        env.settings.releaseStatus = OSFSettings::API::Status::Ok;
        runtime().Update();
        CheckReleased();
        CHECK(env.settings.releases == releases + 1);
    }

    void Recovery(bool healthyDevice)
    {
        PluginLoad();
        PostLoad();
        DataReady();
        LoadHud();
        CHECK(api().SetState("acme", "native", "42"));
        OpenMenu();
        env.engineFocus = true;
        runtime().Update();
        runtime().EnqueueRelativePointerCapture("acme/menu", true);
        runtime().Update();
        CHECK(pointer == std::vector<API::RelativePointerPhase>{ API::RelativePointerPhase::kBegin });
        CHECK(runtime().OnGameWindowMouseRelative(4, 5));

        // Model an already displayed GPU frame at the compositor boundary. Failure
        // must explicitly hide it; a default-false double would make this check vacuous.
        env.compositorVisible = true;

        QueueFailure("capture-ring", 0x887A0005u);
        QueueFailure("host-connection"); // Duplicate transport death must not double-release.
        runtime().Update();
        CheckReleased();
        CHECK(!runtime().IsVisible() && !env.compositorVisible);
        CHECK(env.hidden.at("acme/menu") && env.hidden.at("acme/hud"));
        CHECK(!api().IsReady());
        // The final accumulated motion is delivered before the one cancellation edge.
        CHECK((pointer == std::vector<API::RelativePointerPhase>{ API::RelativePointerPhase::kBegin,
            API::RelativePointerPhase::kUpdate, API::RelativePointerPhase::kCancel }));
        CHECK(!runtime().OnGameWindowMouseRelative(1, 1));
        CHECK(CountLifecycle("acme/menu", API::ViewLifecyclePhase::kHidden) == 1);
        CHECK(env.settings.releases == 1 && env.restarts == 0);

        env.gameDeviceHealthy = healthyDevice;
        // Runtime uses the real steady clock. Only wait for its first 1s recovery delay;
        // exact backoff/budget edges are covered with synthetic time in browser_host_recovery_tests.
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
        while (env.deviceReleases == 0 && std::chrono::steady_clock::now() < deadline) {
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
            runtime().Update();
        }
        CHECK(env.deviceReleases == 1 && env.queueReleases == 1);
        if (!healthyDevice) {
            CHECK(env.restarts == 0 && !api().IsReady());
            CHECK(env.diagnostics.Get("runtime.renderer").nextSteps.find("Restart Starfield") != std::string::npos);
            CheckReleased();
            return;
        }
        CHECK(env.restarts == 1);
        CHECK(std::ranges::count(env.navigations, "acme/menu") == 2);
        CHECK(std::ranges::count(env.navigations, "acme/hud") == 2);
        CHECK(env.hidden.at("acme/hud") && env.hidden.at("acme/menu"));
        env.sent.clear();
        QueueLoad("acme/hud");
        QueueHello("acme/hud");
        QueueLoad("acme/menu");
        QueueHello("acme/menu");
        runtime().Update();
        runtime().Update(); // Admission observes health on the following lifecycle pass.
        CHECK(api().IsReady());
        CHECK(!env.diagnostics.Has("runtime.renderer"));
        CHECK(!env.hidden.at("acme/hud") && env.hidden.at("acme/menu"));
        CHECK(StateFor("acme/menu", "native") == 42 && StateFor("acme/hud", "native") == 42);
        CHECK(CountLifecycle("acme/menu", API::ViewLifecyclePhase::kShown) == 1); // Menus never reopen on recovery.
        CheckReleased();
    }
}

int main(int argc, char** argv)
{
    // One scenario per process: Runtime and BridgeApi deliberately have process lifetimes.
    const std::string_view scenario = argc > 1 ? argv[1] : "normal";
    FixtureFiles files;
    SettingsTest::Install(&env.settings, &env.diagnostics);
    if (scenario == "cef") {
        std::ofstream(env.dataDir / "backend.json") << R"({"schemaVersion":1,"backend":"cef"})";
        expectedCef = true;
        NormalLifecycle();
    }
    else if (scenario == "normal") {
        env.settings.values["cefPrototype"] = true; // Obsolete setting cannot override the installed edition.
        NormalLifecycle();
    }
    else if (scenario == "missing-settings" || scenario == "renderer-init-failure") StartupFailure(scenario);
    else if (scenario == "input-hook-failure" || scenario == "focus-menu-failure" || scenario == "menu-events-failure") InputFailure(scenario);
    else if (scenario == "suppression-failure") SuppressionFailure();
    else if (scenario == "recovery" || scenario == "game-device-failure") Recovery(scenario == "recovery");
    else CHECK(false);
    std::printf("runtime_lifecycle_tests (%.*s): %d checks, %d failures\n", static_cast<int>(scenario.size()), scenario.data(), g_checks, g_failures);
    return g_failures ? 1 : 0;
}
