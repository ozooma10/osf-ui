#include "Runtime/Runtime.h"
#include "RuntimeDoubles.h"
#include "BrowserMessages.h"
#include "check.h"

#include <thread>

using namespace OSFUI;
using namespace RuntimeTest;
namespace RuntimeTest { extern std::atomic_bool surfaceGpuIdle, surfaceGpuReady; }
extern "C" void* OSFUI_RequestSurfaces(std::uint32_t, std::uint32_t*) noexcept;

namespace
{
    struct FixtureFiles
    {
        FixtureFiles()
        {
            const auto suffix = std::chrono::steady_clock::now().time_since_epoch().count();
            env.dataDir = std::filesystem::temp_directory_path() / ("osfui-lifecycle-" + std::to_string(suffix));
            if (!std::filesystem::create_directory(env.dataDir)) throw std::runtime_error("fixture directory already exists");
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
        env.restartPending[""] = true;
        runtime().Update();
        CHECK(env.restarts == 1 && !api().IsReady() && !runtime().IsVisible());
        env.restartPending[""] = false;
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

void SurfaceLifecycle()
{
    FixtureFiles::Write("acme/screen", R"({"manifestVersion":1,"kind":"world","texture":"textures/osfui/feeds/test.dds","width":1280,"height":720})");
    PluginLoad(); PostLoad();
    auto& service = SurfaceService::Get();
    const auto initial = env.rendererInitializations;
    CHECK(!service.IsInstantiated("acme/screen"));
    CHECK(service.Acquire("acme/menu") == 0);
    std::uint32_t actual=99;
    CHECK(OSFUI_RequestSurfaces(0x00020000,&actual) == nullptr && actual == 0);
    CHECK(OSFUI_RequestSurfaces(API::Surfaces::kVersion,&actual) == &service && actual == API::Surfaces::kVersion);
    CHECK(OSFUI_RequestSurfaces(0x00010000,&actual) == &service);
    CHECK(OSFUI_RequestSurfaces(0x00010001,&actual) == &service);
    CHECK(OSFUI_RequestSurfaces(0x00010002,&actual) == &service);
    const auto first=service.Acquire("acme/screen"), second=service.Acquire("ACME/SCREEN");
    CHECK(first && second && first!=second);
    service.Update(true);
    CHECK(env.rendererInitializations == initial+1);
    CHECK(service.IsInstantiated("acme/screen"));
    QueueLoad("acme/screen"); service.Update(true);
    std::vector<API::Surfaces::Phase> phases;
    const auto callback=+[](std::uint64_t,API::Surfaces::Phase phase,const char*,void* context) noexcept {
        static_cast<std::vector<API::Surfaces::Phase>*>(context)->push_back(phase);
    };
    auto token=service.BeginInteraction(first,callback,&phases);
    CHECK(token);
    surfaceGpuReady=false;
    service.UpdateInteraction(true,true,true,[]{return false;});
    CHECK(phases.empty() && !service.InputRenderer());
    surfaceGpuReady=true;
    service.UpdateInteraction(true,false,true,[]{return false;});
    service.UpdateInteraction(true,true,true,[]{return false;});
    CHECK(phases.empty());
    service.UpdateInteraction(true,true,true,[]{return false;});
    CHECK((phases == std::vector{API::Surfaces::Phase::Rejected}));
    phases.clear();
    token=service.BeginInteraction(first,callback,&phases);
    auto admit=[] { env.settings.blocks.insert(777); return true; };
    service.UpdateInteraction(true,true,true,admit); service.UpdateInteraction(true,true,true,admit);
    CHECK((phases == std::vector{API::Surfaces::Phase::Started}));
    CHECK(service.InputView() == "acme/screen");
    service.Release(first); service.UpdateInteraction(true,true,true,admit);
    CHECK((phases == std::vector{API::Surfaces::Phase::Started,API::Surfaces::Phase::Ended}));
    CHECK(!service.InputRenderer());
    service.Update(true);
    CHECK(service.IsInstantiated("acme/screen")); // second lease keeps its browser
    CHECK(env.rendererInitializations == initial+1);
    phases.clear();
    token=service.BeginInteraction(second,callback,&phases);
    CHECK(service.EndInteraction(token));
    service.UpdateInteraction(true,true,true,admit);
    CHECK((phases == std::vector{API::Surfaces::Phase::Rejected}));
    phases.clear();
    CHECK(service.BeginInteraction(second,callback,&phases));
    service.CancelInteraction(); // cancels the queued request, including before its drain
    CHECK(service.BeginInteraction(second,callback,&phases)); // later requests survive
    service.UpdateInteraction(true,true,true,admit); service.UpdateInteraction(true,true,true,admit);
    CHECK((phases == std::vector{API::Surfaces::Phase::Rejected,API::Surfaces::Phase::Started}));
    service.CancelInteraction(); service.UpdateInteraction(true,true,true,admit);
    CHECK(phases.back() == API::Surfaces::Phase::Ended);
    phases.clear();
    token=service.BeginInteraction(second,callback,&phases);
    service.UpdateInteraction(true,true,true,admit); service.UpdateInteraction(true,true,true,admit);
    CHECK(service.BeginInteraction(second,callback,&phases)); // queued behind the active one
    CHECK(service.EndInteraction(token));
    service.UpdateInteraction(true,true,true,admit); service.UpdateInteraction(true,true,true,admit);
    CHECK((phases == std::vector{API::Surfaces::Phase::Started,API::Surfaces::Phase::Ended,API::Surfaces::Phase::Started})); // ending one keeps the queue
    // A transient input owner may outlive service detachment. Retirement must
    // explicitly stop the host before dropping its own strong reference.
    auto retainedHost=service.InputRenderer();
    CHECK(retainedHost);
    const auto shutdowns=env.rendererShutdowns.load(), destructions=env.rendererDestructions.load();
    service.CancelInteraction(); service.UpdateInteraction(true,true,true,admit);
    CHECK(phases.back() == API::Surfaces::Phase::Ended);
    surfaceGpuIdle=false; service.Release(second); service.Update(true);
    CHECK(service.IsInstantiated("acme/screen")); // GPU reads retain the host
    surfaceGpuIdle=true; service.Update(true);
    CHECK(!service.IsInstantiated("acme/screen"));
    auto deadline=std::chrono::steady_clock::now()+std::chrono::seconds(1);
    while (env.rendererShutdowns.load()==shutdowns && std::chrono::steady_clock::now()<deadline)
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    CHECK(env.rendererShutdowns.load()==shutdowns+1 && env.rendererDestructions.load()==destructions);
    retainedHost.reset();
    deadline=std::chrono::steady_clock::now()+std::chrono::seconds(1);
    while (env.rendererDestructions.load()==destructions && std::chrono::steady_clock::now()<deadline)
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    CHECK(env.rendererDestructions.load()==destructions+1 && env.rendererShutdowns.load()==shutdowns+1);
    service.Release(second); // stale releases are harmless
}

void SurfaceReloadLifecycle()
{
    FixtureFiles::Write("worldonly/screen", R"({"manifestVersion":1,"kind":"world","texture":"textures/osfui/feeds/worldonly.dds"})");
    FixtureFiles::Write("acme/screen", R"({"manifestVersion":1,"kind":"world","texture":"textures/osfui/feeds/acme.dds"})");
    env.settings.values["developerMode"] = true;
    PluginLoad(); PostLoad(); DataReady(); LoadHud();
    auto& service = SurfaceService::Get();
    const auto world = service.Acquire("worldonly/screen"), mixed = service.Acquire("acme/screen");
    runtime().Update();
    QueueLoad("worldonly/screen"); QueueHello("worldonly/screen");
    QueueLoad("acme/screen"); QueueHello("acme/screen"); runtime().Update();
    CHECK(std::ranges::find(env.reloadMods,"worldonly") != env.reloadMods.end());
    CHECK(std::ranges::find(env.reloadMods,"acme") != env.reloadMods.end());
    CHECK(env.refreshMod);
    // Refresh the separate loose-file mirrors before announcing completion.
    CHECK(env.refreshMod("worldonly"));
    CHECK((env.refreshedMods == std::vector<std::pair<std::string,std::string>>{
        {"","worldonly"},{"surface-worldonly!screen","worldonly"}}));
    env.refreshFailsHost = "surface-worldonly!screen";
    CHECK(!env.refreshMod("worldonly"));
    env.refreshFailsHost.clear();
    CHECK(std::ranges::count(env.navigations,"worldonly/screen") == 1);
    std::vector<API::Surfaces::Phase> phases;
    const auto callback=+[](std::uint64_t,API::Surfaces::Phase phase,const char*,void* context) noexcept {
        static_cast<std::vector<API::Surfaces::Phase>*>(context)->push_back(phase);
    };
    CHECK(service.BeginInteraction(world,callback,&phases));
    runtime().Update(); runtime().Update();
    CHECK(runtime().IsInputCaptured());
    env.message("worldonly/screen",R"({"kind":"send","name":"osfui.gamepadMode","payload":{"mode":"raw"}})");
    env.reloadCompleted = {"worldonly"}; runtime().Update();
    CheckReleased();
    CHECK((phases == std::vector{API::Surfaces::Phase::Started,API::Surfaces::Phase::Ended}));
    CHECK(std::ranges::count(env.navigations,"worldonly/screen") == 2);
    CHECK(std::ranges::count(env.navigations,"acme/screen") == 1);
    CHECK(std::ranges::count(env.navigations,"acme/hud") == 1);
    CHECK(env.viewHosts.at("worldonly/screen") == "surface-worldonly!screen");
    CHECK(service.LoadState("worldonly/screen") == std::string_view("loading"));
    QueueLoad("worldonly/screen"); QueueHello("worldonly/screen"); runtime().Update();
    phases.clear(); CHECK(service.BeginInteraction(world,callback,&phases));
    runtime().Update(); runtime().Update();
    // Reload discards document-specific raw input grants.
    env.gamepad.buttons=XInputButton::kB; runtime().Update(); runtime().Update();
    CheckReleased(); env.gamepad={};
    CHECK((phases == std::vector{API::Surfaces::Phase::Started,API::Surfaces::Phase::Ended}));
    env.refreshedMods.clear(); CHECK(env.refreshMod("acme"));
    CHECK((env.refreshedMods == std::vector<std::pair<std::string,std::string>>{
        {"","acme"},{"surface-acme!screen","acme"}}));
    env.reloadCompleted={"acme"}; runtime().Update();
    CHECK(std::ranges::count(env.navigations,"acme/hud") == 2);
    CHECK(std::ranges::count(env.navigations,"acme/screen") == 2);
    CHECK(std::ranges::count(env.navigations,"acme/second") == 0);
    CHECK(std::ranges::count(env.navigations,"worldonly/screen") == 2);

    // A reload rejects requests for the replaced document before admission,
    // without cancelling a queued request belonging to another world view.
    phases.clear();
    std::vector<API::Surfaces::Phase> otherPhases;
    CHECK(service.BeginInteraction(world,callback,&phases));
    const auto otherInteraction = service.BeginInteraction(mixed,callback,&otherPhases);
    CHECK(otherInteraction);
    env.reloadCompleted={"worldonly"}; runtime().Update();
    CHECK((phases == std::vector{API::Surfaces::Phase::Rejected}));
    QueueLoad("acme/screen"); QueueHello("acme/screen");
    runtime().Update(); runtime().Update();
    CHECK((otherPhases == std::vector{API::Surfaces::Phase::Started}));
    CHECK(service.EndInteraction(otherInteraction)); runtime().Update(); CheckReleased();
    QueueLoad("worldonly/screen"); QueueHello("worldonly/screen"); runtime().Update();
    phases.clear(); CHECK(service.BeginInteraction(world,callback,&phases));
    runtime().Update(); // Accepted request is still waiting for its second neutral tick.
    CHECK(service.WantsInteraction()); CHECK(!runtime().IsInputCaptured());
    env.reloadCompleted={"worldonly"}; runtime().Update();
    CHECK((phases == std::vector{API::Surfaces::Phase::Rejected}));
    CheckReleased(); CHECK(!service.WantsInteraction());

    // Simulate the runtime tick while the file worker owns its refresh pin.
    // Releasing/reacquiring demand cannot detach the host or reuse its profile.
    API::Surfaces::Handle replacementWorld{};
    const auto initialized = env.rendererInitializations;
    env.duringRefreshMod = [&](std::string_view host) {
        CHECK(host == "surface-worldonly!screen");
        service.Release(world); service.Update(true);
        CHECK(service.IsInstantiated("worldonly/screen"));
        replacementWorld=service.Acquire("worldonly/screen"); CHECK(replacementWorld);
        service.Update(true);
        CHECK(env.rendererInitializations == initialized);
    };
    CHECK(service.RefreshModFiles("worldonly"));
    env.duringRefreshMod={};
    const auto worldNavigations=std::ranges::count(env.navigations,"worldonly/screen");
    service.Release(replacementWorld); runtime().Update();
    CHECK(!service.IsInstantiated("worldonly/screen"));
    CHECK(std::ranges::find(env.reloadMods,"worldonly") == env.reloadMods.end());
    // A completed worker refresh for a retired surface cannot resurrect it.
    env.reloadCompleted={"worldonly"}; runtime().Update();
    CHECK(std::ranges::count(env.navigations,"worldonly/screen") == worldNavigations);

    // Failed refreshes release every pin and remain retryable on the worker.
    const auto mixedNavigations=std::ranges::count(env.navigations,"acme/screen");
    env.duringRefreshMod=[](std::string_view) { throw std::runtime_error("injected refresh failure"); };
    CHECK(!service.RefreshModFiles("acme"));
    env.duringRefreshMod={};
    CHECK(std::ranges::count(env.navigations,"acme/screen") == mixedNavigations);
    service.Release(mixed); runtime().Update();
    CHECK(!service.IsInstantiated("acme/screen"));
}

void SurfaceInputLifecycle()
{
    FixtureFiles::Write("acme/screen", R"({"manifestVersion":1,"kind":"world","texture":"textures/osfui/feeds/test.dds"})");
    PluginLoad(); PostLoad(); DataReady(); LoadHud();
    const auto overlayFailure=env.failure;
    auto& service = SurfaceService::Get();
    const auto lease=service.Acquire("acme/screen");
    runtime().Update(); QueueLoad("acme/screen"); QueueHello("acme/screen"); runtime().Update();
    CHECK(service.HasAvailableViews() && service.LoadState("acme/screen") == std::string_view("loaded"));
    std::vector<API::Surfaces::Phase> phases;
    const auto callback=+[](std::uint64_t,API::Surfaces::Phase phase,const char*,void* context) noexcept {
        static_cast<std::vector<API::Surfaces::Phase>*>(context)->push_back(phase);
    };
    const auto begin=[&] {
        const auto beforeAdmission=env.keyNeutralChecks;
        CHECK(service.BeginInteraction(lease,callback,&phases));
        env.keysNeutral=false; runtime().Update(); runtime().Update();
        CHECK(!runtime().IsInputCaptured());
        CHECK(env.keyNeutralChecks >= beforeAdmission+2);
        env.keysNeutral=true; runtime().Update(); runtime().Update();
        CHECK(runtime().IsInputCaptured() && env.controlsBlocked && !env.paused && !env.cursorFree);
        const auto admitted=env.keyNeutralChecks;
        env.engineFocus=true; runtime().Update();
        CHECK(env.browserFocus && env.inputTarget=="acme/screen");
        env.keysNeutral=false; runtime().Update(); runtime().Update();
        CHECK(runtime().IsInputCaptured() && env.keyNeutralChecks == admitted);
        env.keysNeutral=true;
    };
    // Loading or focus changes before activation cannot poison its first try.
    service.CancelInteraction();
    runtime().Update();
    begin();
    // Idempotent HUD opens, newly shown HUDs, and refused launcher requests do not own input.
    runtime().EnqueueOpenView("acme/hud"); runtime().Update();
    CHECK(runtime().IsInputCaptured() && phases.size()==1);
    runtime().EnqueueCloseView("acme/hud"); runtime().Update();
    runtime().EnqueueOpenView("acme/hud"); runtime().Update();
    CHECK(runtime().IsInputCaptured() && phases.size()==1 && !env.hidden.at("acme/hud"));
    api().ViewRequests().EnqueueLauncherOpen("acme/hud",1); runtime().Update();
    CHECK(runtime().IsInputCaptured() && phases.size()==1);
    // An admitted overlay menu still relinquishes surface input while its page loads.
    runtime().EnqueueOpenView("acme/menu"); runtime().Update();
    CheckReleased();
    CHECK((phases==std::vector{API::Surfaces::Phase::Started,API::Surfaces::Phase::Ended}));
    runtime().EnqueueCloseView("acme/menu"); runtime().Update();
    phases.clear(); begin();
    CHECK(runtime().OnGameWindowKeyboard({.vk=0x1B,.down=true}));
    runtime().Update(); CheckReleased();
    CHECK((phases==std::vector{API::Surfaces::Phase::Started,API::Surfaces::Phase::Ended}));
    phases.clear(); begin();
    env.message("acme/screen",R"({"kind":"send","name":"osfui.gamepadMode","payload":{"mode":"raw"}})");
    env.gamepad.buttons=XInputButton::kBack;
    runtime().Update(); runtime().Update(); CheckReleased();
    CHECK((phases==std::vector{API::Surfaces::Phase::Started,API::Surfaces::Phase::Ended}));
    phases.clear(); env.gamepad={}; begin();
    env.message("acme/screen",R"({"kind":"send","name":"osfui.gamepadMode","payload":{"mode":"raw"}})");
    const auto waitForRestart=[&](int expected) {
        const auto deadline=std::chrono::steady_clock::now()+std::chrono::seconds(5);
        while (env.restarts<expected && std::chrono::steady_clock::now()<deadline) {
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
            runtime().Update();
        }
        CHECK(env.restarts==expected);
    };
    // Recovery belongs to the failed host: the healthy surface keeps its document and raw mode.
    QueueFailure("host-connection");
    runtime().Update();
    waitForRestart(1);
    QueueLoad("acme/hud"); QueueHello("acme/hud"); runtime().Update(); runtime().Update();
    CHECK(std::ranges::count(env.navigations,"acme/hud")==2);
    CHECK(std::ranges::count(env.navigations,"acme/screen")==1);
    env.gamepad.buttons=XInputButton::kB; runtime().Update(); runtime().Update();
    CHECK(runtime().IsInputCaptured() && phases.size()==1);
    env.gamepad={}; runtime().Update();
    // Replacing the surface's own document cancels input and clears its previous raw-mode grant.
    QueueFailure("host-connection",0,"surface-acme!screen"); runtime().Update();
    CheckReleased();
    CHECK((phases==std::vector{API::Surfaces::Phase::Started,API::Surfaces::Phase::Ended}));
    waitForRestart(2);
    env.restartPending["surface-acme!screen"] = true;
    runtime().Update();
    CHECK(env.restarts == 2 && !runtime().IsInputCaptured());
    env.restartPending["surface-acme!screen"] = false;
    QueueLoad("acme/screen"); QueueHello("acme/screen"); runtime().Update();
    CHECK(std::ranges::count(env.navigations,"acme/screen")==2);
    phases.clear(); begin();
    env.gamepad.buttons=XInputButton::kB; runtime().Update(); runtime().Update();
    CheckReleased();
    CHECK((phases==std::vector{API::Surfaces::Phase::Started,API::Surfaces::Phase::Ended}));
    phases.clear(); env.gamepad={}; begin();
    // An unavailable overlay rejects its menu open without canceling the healthy surface.
    overlayFailure({.stage="test-permanent",.description="EXPECTED TEST: overlay unavailable"});
    runtime().EnqueueOpenView("acme/menu"); runtime().Update();
    CHECK(runtime().IsInputCaptured() && phases.size()==1);
    env.gameWindowActive=false; runtime().Update(); CheckReleased();
    CHECK((phases==std::vector{API::Surfaces::Phase::Started,API::Surfaces::Phase::Ended}));
    service.Release(lease); runtime().Update();
}

void SurfaceFullscreenLifecycle()
{
    FixtureFiles::Write("acme/tv", R"({"manifestVersion":1,"kind":"world","texture":"textures/osfui/feeds/tv.dds","width":1280,"height":720,"fullscreen":true})");
    FixtureFiles::Write("acme/screen", R"({"manifestVersion":1,"kind":"world","texture":"textures/osfui/feeds/test.dds"})");
    PluginLoad(); PostLoad(); DataReady(); LoadHud();
    auto& service = SurfaceService::Get();
    const auto screen=service.Acquire("acme/screen");
    runtime().Update();
    struct Seen { std::vector<API::Surfaces::Phase> phases; std::string reason; } seen;
    const auto callback=+[](std::uint64_t,API::Surfaces::Phase phase,const char* reason,void* context) noexcept {
        auto& s=*static_cast<Seen*>(context); s.phases.push_back(phase); s.reason=reason ? reason : "";
    };
    const API::Surfaces::InteractionOptions fullscreen{.presentation=API::Surfaces::Presentation::Fullscreen};
    CHECK(!service.BeginInteractionWith(screen,{.size=4,.presentation=API::Surfaces::Presentation::Fullscreen},callback,&seen));

    // Views must opt in to fullscreen.
    CHECK(service.BeginInteractionWith(screen,fullscreen,callback,&seen));
    runtime().Update();
    CHECK((seen.phases==std::vector{API::Surfaces::Phase::Rejected}) && seen.reason=="fullscreen-unsupported");
    seen={};
    service.Release(screen); runtime().Update();
    CHECK(!service.IsInstantiated("acme/screen"));

    const auto tv=service.Acquire("acme/tv");
    runtime().Update(); QueueLoad("acme/tv"); QueueHello("acme/tv"); runtime().Update();
    CHECK(service.LoadState("acme/tv") == std::string_view("loaded"));

    CHECK(service.BeginInteractionWith(tv,fullscreen,callback,&seen));
    runtime().Update(); runtime().Update(); env.engineFocus=true; runtime().Update();
    CHECK((seen.phases==std::vector{API::Surfaces::Phase::Started}));
    CHECK(runtime().IsInputCaptured() && service.FullscreenActive() && env.inputTarget=="acme/tv");
    CHECK(env.paused && env.cursorFree && env.pointerInput && env.compositorFullscreen);

    // A 16:9 surface on a 16:10 client: 80 px bars above and below at 2560x1600.
    runtime().OnGameWindowMouseAbsolute(1280,800,2560,1600);
    runtime().OnGameWindowMouseButton(0,true); runtime().OnGameWindowMouseButton(0,false);
    CHECK(env.mouse.size()==2 && env.mouse[0].x==640 && env.mouse[0].y==360 && env.mouse[0].down && !env.mouse[1].down);
    env.mouse.clear();
    runtime().OnGameWindowMouseAbsolute(1280,40,2560,1600); // top bar
    runtime().OnGameWindowMouseButton(0,true); runtime().OnGameWindowMouseButton(0,false);
    CHECK(env.mouse.size()==1 && !env.mouse[0].down); // bars swallow presses, never releases

    CHECK(runtime().OnGameWindowKeyboard({.vk=0x1B,.down=true}));
    runtime().Update(); CheckReleased();
    CHECK((seen.phases==std::vector{API::Surfaces::Phase::Started,API::Surfaces::Phase::Ended}));
    CHECK(!service.FullscreenActive() && !env.pointerInput && !env.compositorFullscreen);
    env.mouse.clear();
    runtime().OnGameWindowMouseButton(0,true);
    CHECK(env.mouse.empty());

    // In-world interactions on the same view stay pointerless and unpaused.
    seen={};
    CHECK(service.BeginInteraction(tv,callback,&seen));
    runtime().Update(); runtime().Update();
    CHECK((seen.phases==std::vector{API::Surfaces::Phase::Started}));
    CHECK(!service.FullscreenActive() && !env.paused && !env.cursorFree && !env.pointerInput && !env.compositorFullscreen);
    service.CancelInteraction(); runtime().Update(); CheckReleased();
    service.Release(tv); runtime().Update();
}

void BrowserSurfaceLifecycle(bool enabled)
{
    namespace msg = osfui::browser::msg;
    constexpr auto view="osfui.browser/tv", content="osfui.browser/content", host="surface-osfui.browser!tv";
    FixtureFiles::Write(view, R"({"manifestVersion":1,"kind":"world","texture":"browser.dds","width":1920,"height":1080,"browser":{"chrome":"chrome.html","chromeHeight":56}})");
    PluginLoad(); PostLoad(); DataReady(); LoadHud();
    auto& service=SurfaceService::Get();
    MessageBridge bridge([](std::string_view id,std::string_view text) { env.sent.push_back({std::string(id),nlohmann::json::parse(text)}); });
    if (enabled) service.Initialize(bridge,false,{}, {},[]{ return true; });
    else service.Initialize(bridge,false,{}, {}); // Production remains default-off without step 6.
    const auto lease=service.Acquire(view);
    CHECK(!service.PublishScreenCount("acme/menu",2));
    CHECK(!service.PublishScreenCount(nullptr,2));
    CHECK(!service.PublishScreenCount(view,4097));
    CHECK(service.PublishScreenCount(view,2)); // Two bindings share this single feed lease.
    runtime().Update(); QueueLoad(view); QueueHello(view); runtime().Update();
    CHECK(env.remoteMode.at(host) == enabled);
    CHECK(env.viewHosts.contains(content) == enabled);
    CHECK(env.browserLayouts.back().landing && !env.browserLayouts.back().fullscreen);
    CHECK(env.browserLayouts.back().content == (enabled ? content : ""));
    API::Surfaces::AudioScene scene;
    scene.count=2; scene.emitters[0]={11,{0,2,0}}; scene.emitters[1]={12,{2,0,0}};
    CHECK(!service.PublishAudioScene("acme/menu",scene));
    CHECK(service.PublishAudioScene(view,scene));
    runtime().Update();
    CHECK(env.browserAudio.size()==(enabled ? 1u : 0u));
    if (enabled) {
        CHECK(env.browserAudio.back().scene["emitters"].size()==2);
        runtime().Update(); CHECK(env.browserAudio.size()==1); // Do not refresh stale producer data.
        scene.count=1; CHECK(service.PublishAudioScene(view,scene));
        runtime().Update(); CHECK(env.browserAudio.back().scene["emitters"].size()==1);
        scene.count=99; CHECK(!service.PublishAudioScene(view,scene));
        runtime().Update(); CHECK(env.browserAudio.back().scene["emitters"].empty()); // Invalid fails silent.
        scene.count=0; CHECK(service.PublishAudioScene(view,scene));
        runtime().Update(); CHECK(env.browserAudio.back().scene["emitters"].empty());
    }
    const auto request=[&](std::string_view source,std::string id,std::string action,nlohmann::json payload=nlohmann::json::object()) {
        bridge.HandleWebMessage(source,nlohmann::json{{"kind","request"},{"id",id},{"name","browser."+action},{"payload",payload}}.dump());
    };
    const auto reply=[&](std::string_view id) {
        for (auto it=env.sent.rbegin(); it != env.sent.rend(); ++it)
            if (it->body.value("id","") == id) return it->body;
        return nlohmann::json::object();
    };
    request(view,"state","getState");
    CHECK(reply("state")["payload"]["enabled"] == enabled);
    CHECK(reply("state")["payload"]["screens"] == 2);
    const auto beforeCountChange=env.sent.size();
    CHECK(service.PublishScreenCount(view,3));
    runtime().Update();
    CHECK(std::ranges::any_of(env.sent.begin()+beforeCountChange,env.sent.end(),[](const auto& message) {
        return message.body.value("name","") == "browser.state" && message.body["payload"]["screens"] == 3;
    }));
    const auto afterCountChange=env.sent.size();
    CHECK(service.PublishScreenCount(view,3)); runtime().Update();
    CHECK(env.sent.size() == afterCountChange); // Unchanged counts do not flood chrome.
    CHECK(service.PublishScreenCount(view,1)); runtime().Update();
    request("acme/menu","forbidden","navigate",{{"input","example.com"}});
    CHECK(reply("forbidden")["payload"]["code"] == "browser-forbidden");
    request(view,"nav","navigate",{{"input","space music"}});
    if (!enabled) {
        CHECK(reply("nav")["payload"]["code"] == "browsing-disabled");
        CHECK(env.browserCommands.empty());
        service.Release(lease); runtime().Update(); return;
    }
    CHECK(reply("nav").empty()); // Completion is correlated to the native result, not the send.
    const auto command=env.browserCommands.back();
    CHECK(command.view == content && command.action == "navigate" && command.url == "https://duckduckgo.com/?q=space%20music");
    env.hosts.at(host).browserResult({.token=command.token,.success=true});
    CHECK(reply("nav")["kind"] == "reply");
    const auto count=env.browserCommands.size();
    for (const auto* address : {"http://localhost","file:///secret","https://osfui.example","192.168.1.1"}) {
        request(view,"bad","navigate",{{"input",address}});
        CHECK(reply("bad")["payload"]["code"] == "invalid-url");
    }
    CHECK(env.browserCommands.size() == count);
    request(view,"mute-bad","mute",{{"muted","yes"}});
    CHECK(reply("mute-bad")["payload"]["code"] == "invalid-payload");
    request(view,"mute","mute",{{"muted",true}});
    CHECK(env.browserCommands.back().action == "mute" && env.browserCommands.back().value);
    env.hosts.at(host).browserResult({.token=env.browserCommands.back().token,.success=false,.error="native failure"});
    CHECK(reply("mute")["payload"]["code"] == "browser-command-failed");
    env.hosts.at(host).browserState({.view=content,.url="https://example.com/video",.title="Native title",.canGoBack=true,.secure=true});
    request(view,"state2","getState");
    CHECK(reply("state2")["payload"]["url"] == "https://example.com/video");
    CHECK(reply("state2")["payload"]["canGoBack"] == true && reply("state2")["payload"]["landing"] == false);
    CHECK(!env.browserLayouts.back().landing);
    // A content-page bridge injection cannot reach chrome's endpoint.
    const auto sent=env.sent.size();
    env.hosts.at(host).message(content,R"({"kind":"request","name":"browser.close","id":"attack","payload":{}})");
    CHECK(env.sent.size() == sent && reply("attack").empty());

    std::vector<API::Surfaces::Phase> phases;
    const auto callback=+[](std::uint64_t,API::Surfaces::Phase phase,const char*,void* context) noexcept {
        static_cast<std::vector<API::Surfaces::Phase>*>(context)->push_back(phase);
    };
    const auto navigationCount=env.navigations.size();
    CHECK(service.BeginInteractionWith(lease,{.presentation=API::Surfaces::Presentation::Fullscreen},callback,&phases));
    runtime().Update(); runtime().Update(); env.engineFocus=true; runtime().Update();
    CHECK(service.FullscreenActive() && env.browserLayouts.back().fullscreen);
    surfaceGpuReady=false; runtime().Update();
    CHECK(service.FullscreenActive()); // The new layout can be between presentations.
    surfaceGpuReady=true;
    service.RequestBack(); runtime().Update();
    CHECK(service.FullscreenActive()); // Page-fullscreen may consume Back; wait for native fallback.
    env.hosts.at(host).back(view,1); runtime().Update();
    CHECK(!service.FullscreenActive() && !env.browserLayouts.back().fullscreen);
    CHECK((phases == std::vector{API::Surfaces::Phase::Started,API::Surfaces::Phase::Ended}));
    CHECK(env.navigations.size() == navigationCount); // Reflow does not recreate either document.
    CHECK(service.IsInstantiated(view));
    CHECK(service.BeginInteractionWith(lease,{.presentation=API::Surfaces::Presentation::Fullscreen},callback,&phases));
    runtime().Update(); runtime().Update(); env.engineFocus=true; runtime().Update();
    request(view,"guide-bad","guide",{{"open","yes"}});
    CHECK(reply("guide-bad")["payload"]["code"] == "invalid-payload");
    request(view,"guide","guide",{{"open",true}});
    CHECK(reply("guide")["kind"] == "reply" && env.browserLayouts.back().landing && env.browserLayouts.back().fullscreen);
    request(view,"state-guide","getState");
    CHECK(reply("state-guide")["payload"]["guide"] == true && reply("state-guide")["payload"]["screens"] == 1);
    service.RequestBack(); runtime().Update();
    CHECK(service.FullscreenActive() && !env.browserLayouts.back().landing); // Back leaves the guide first.
    request(view,"guide2","guide",{{"open",true}});
    CHECK(env.browserLayouts.back().landing);
    request(view,"nav2","navigate",{{"input","example.org"}});
    CHECK(!env.browserLayouts.back().landing); // Choosing a channel returns to the page.
    env.hosts.at(host).browserResult({.token=env.browserCommands.back().token,.success=true});
    request(view,"guide3","guide",{{"open",true}});
    request(view,"close","close"); runtime().Update();
    CHECK(reply("close")["kind"] == "reply" && !service.FullscreenActive());
    CHECK(!env.browserLayouts.back().landing); // Leaving the browser closes the guide.
    request(view,"guide-idle","guide",{{"open",true}});
    CHECK(reply("guide-idle")["payload"]["code"] == "browser-not-interactive");
    CHECK(env.navigations.size() == navigationCount);
    request(view,"clear","clearData");
    env.hosts.at(host).browserResult({.token=env.browserCommands.back().token,.success=true,.restartRequired=true});
    CHECK(reply("clear")["payload"]["restartRequired"] == true);
    env.hosts.at(host).load({.viewId=content,.failed=true,.description="Site failed"});
    CHECK(service.LoadState(view) == std::string_view("loaded") && env.browserLayouts.back().landing);
    request(view,"state3","getState");
    CHECK(reply("state3")["payload"]["error"] == "Site failed");
    request(view,"pending","reload");
    service.Release(lease); runtime().Update();
    CHECK(reply("pending")["payload"]["code"] == "browser-unavailable");
    const auto replies=env.sent.size();
    env.hosts.at(host).browserResult({.token=env.browserCommands.back().token,.success=true});
    CHECK(env.sent.size() == replies); // Late completion cannot revive a retired request.
}

void BrowserConsentLifecycle(bool enabled, bool failedRead)
{
    constexpr auto view="osfui.browser/tv", content="osfui.browser/content", host="surface-osfui.browser!tv";
    FixtureFiles::Write(view, R"({"manifestVersion":1,"kind":"world","texture":"browser.dds","browser":{"chrome":"chrome.html"}})");
    FixtureFiles::Write("acme/screen", R"({"manifestVersion":1,"kind":"world","texture":"plain.dds"})");
    env.settings.values["allowWebBrowsing"]=enabled;
    if (failedRead) env.settings.readStatus["allowWebBrowsing"]=OSFSettings::API::Status::TypeMismatch;
    PluginLoad(); PostLoad(); DataReady(); LoadHud();
    const bool allowed=enabled && !failedRead;
    // Changing a restart setting after startup must not change the launch policy.
    env.settings.values["allowWebBrowsing"]=!enabled;
    env.settings.readStatus.erase("allowWebBrowsing");
    auto& service=SurfaceService::Get();
    const auto lease=service.Acquire(view), plain=service.Acquire("acme/screen");
    runtime().Update(); QueueLoad(view); QueueHello(view); runtime().Update();
    CHECK(env.remoteMode.at(host) == allowed);
    CHECK(env.viewHosts.contains(content) == allowed);
    CHECK(!env.remoteMode.at("") && !env.remoteMode.at("surface-acme!screen")); // Consent is not global network permission.
    CHECK(env.browserLayouts.back().content == (allowed ? content : ""));
    env.hosts.at(host).message(view,R"({"kind":"request","id":"consent-state","name":"browser.getState","payload":{}})");
    bool replied=false;
    for (const auto& message : env.sent) if (message.body.value("id","") == "consent-state") {
        replied=true; CHECK(message.body["payload"]["enabled"] == allowed);
    }
    CHECK(replied);
    env.hosts.at(host).message(view,R"({"kind":"request","id":"consent-nav","name":"browser.navigate","payload":{"input":"example.com"}})");
    CHECK(env.browserCommands.empty() == !allowed);
    if (!allowed) CHECK(env.sent.back().body["payload"]["code"] == "browsing-disabled");
    CHECK(env.diagnostics.Has("settings.allowWebBrowsing") == failedRead);
    service.Release(lease); service.Release(plain); runtime().Update();
}

int main(int argc, char** argv)
{
    // One scenario per process: Runtime and BridgeApi deliberately have process lifetimes.
    const std::string_view scenario = argc > 1 ? argv[1] : "normal";
    expectedCef = argc > 2 && std::string_view(argv[2]) == "cef";
    FixtureFiles files;
    SettingsTest::Install(&env.settings, &env.diagnostics);
    if (scenario == "stale-selection") {
        // Files left by older editions cannot override the compiled backend.
        std::ofstream(env.dataDir / "backend.json") << (expectedCef
            ? R"({"schemaVersion":1,"backend":"webview2"})"
            : R"({"schemaVersion":1,"backend":"cef"})");
        NormalLifecycle();
    }
    else if (scenario == "surface-input") SurfaceInputLifecycle();
    else if (scenario == "surface-reload") SurfaceReloadLifecycle();
    else if (scenario == "surface-fullscreen") SurfaceFullscreenLifecycle();
    else if (scenario == "browser-surface") BrowserSurfaceLifecycle(true);
    else if (scenario == "browser-disabled") BrowserSurfaceLifecycle(false);
    else if (scenario == "browser-consent-on") BrowserConsentLifecycle(true,false);
    else if (scenario == "browser-consent-off") BrowserConsentLifecycle(false,false);
    else if (scenario == "browser-consent-failed") BrowserConsentLifecycle(true,true);
    else if (scenario == "surfaces") SurfaceLifecycle();
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
