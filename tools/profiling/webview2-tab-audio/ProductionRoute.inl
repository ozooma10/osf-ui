// Included after the standalone fixture helpers. Exercises shipping route code.
int RunProductionRoute(View& source, ICoreWebView2Environment* environment,
    std::unique_ptr<osfui::webview2::SpatialAudio>& route, json& report,
    const std::function<void()>& save)
{
    namespace audio = osfui::browser::audio;
    using OSFUI::API::Surfaces::AudioScene;
    using osfui::webview2::SpatialAudio;
    AudioMeter meter;
    if (!meter.Start()) throw std::runtime_error("Production endpoint meter start: " + meter.Snapshot().dump());
    ComPtr<ICoreWebView2_8> sourceAudio;
    Check(source.web.As(&sourceAudio), "Production source native mute control");
    json activeScene;
    bool refreshScene{};
    std::uint64_t nextScene{};
    unsigned routeGeneration = 1;
    auto tick = [&] {
        if (!route) return;
        const auto now = audio::ClockMs();
        if (refreshScene && now >= nextScene) {
            route->Update(activeScene, now + audio::kSceneTimeoutMs);
            nextScene = now + 100;
        }
        route->Tick();
    };
    auto delay = [&](unsigned ms) {
        const auto end = Clock::now() + std::chrono::milliseconds(ms);
        while (Clock::now() < end) { tick(); Pump(); }
    };
    auto sourceState = [&] {
        auto value = source.Script(L"JSON.stringify(window.sourceProbe?.snapshot() ?? {state:'about-blank'})");
        BOOL muted{};
        Check(sourceAudio->get_IsMuted(&muted), "Production source native mute readback");
        value["nativeMuted"] = muted != FALSE;
        return value;
    };
    auto measure = [&](const char* name, unsigned ms = 2000) {
        delay(500);
        if (!meter.Reset()) throw std::runtime_error("Production endpoint meter reset: " + meter.Snapshot().dump());
        delay(ms);
        report["phases"].push_back({{"name", name}, {"endpoint", meter.Snapshot()},
            {"route", route ? route->Status() : json{{"state","closed"}}},
            {"routeGeneration",routeGeneration}, {"scene",activeScene},
            {"refreshScene",refreshScene}, {"source",sourceState()}});
        save();
    };
    auto waitCapture = [&] {
        const auto end = Clock::now() + std::chrono::seconds(16);
        while (Clock::now() < end) {
            tick(); Pump();
            const auto status = route->Status();
            const auto phase = status.value("state", "");
            if (phase == "capturing" || phase == "silent") return;
            if (phase == "failed") throw std::runtime_error("Production capture failed: " + status.dump());
        }
        throw std::runtime_error("Production capture did not reach a running state");
    };
    auto setScene = [&](float x, float y, unsigned count = 1) {
        AudioScene scene;
        scene.count = count;
        if (count) scene.emitters[0] = {1,{x,y,0}};
        activeScene = audio::Encode(scene);
        refreshScene = true;
        nextScene = 0;
        tick();
    };

    route->Start(environment, source.window);
    measure("quiet-background", 1000);
    const auto blankStarted = Clock::now();
    delay(16100);
    report["aboutBlankWaitWallSeconds"] = std::chrono::duration<double>(Clock::now() - blankStarted).count();
    measure("production-idle-about-blank-silent", 1000);
    source.Navigate(kSource);
    route->SetLoading(false);
    if (report.value("remoteVideo", false)) source.Script(L"sourceProbe.prepareVideo(); null");
    source.Script(L"sourceProbe.start(); null");
    waitCapture();
    measure("production-startup-silent");
    setScene(0,1);
    measure("production-center");
    setScene(-4,1);
    measure("production-left");
    setScene(4,1);
    measure("production-right");
    setScene(0,19);
    measure("production-far");
    setScene(0,1);
    route->SetMuted(true);
    measure("production-logical-muted");
    route->SetMuted(false);
    setScene(0,1,0);
    measure("production-empty-scene");
    setScene(0,1);
    refreshScene = false;
    delay(1200);
    measure("production-expired-scene");
    setScene(0,1);
    measure("production-scene-recovered");
    route->SetLoading(true);
    source.Navigate(kSource);
    if (report.value("remoteVideo", false)) source.Script(L"sourceProbe.prepareVideo(); null");
    source.Script(L"sourceProbe.start(); null");
    route->SetLoading(false);
    waitCapture();
    measure("production-navigation-retained");
    refreshScene = false;
    route->Fail("probe-forced-collector-failure");
    measure("production-failed-guard-silent");
    route.reset();
    measure("production-route-closed-silent");

    ++routeGeneration;
    route = std::make_unique<SpatialAudio>(source.web.Get(),
        [&](const std::string& message) { report["productionLogs"].push_back(message); });
    if (!route->Guarded()) throw std::runtime_error("Recreated production route has no native mute guard");
    source.Navigate(kSource);
    route->SetLoading(false);
    if (report.value("remoteVideo", false)) source.Script(L"sourceProbe.prepareVideo(); null");
    source.Script(L"sourceProbe.start(); null");
    route->Start(environment, source.window);
    setScene(0,1);
    waitCapture();
    measure("production-navigation-recreated");
    refreshScene = false;
    route.reset();
    source.Script(L"sourceProbe.stop(); null");
    delay(300);
    report["finalSource"] = sourceState();
    meter.Stop();
    report["result"] = "production-route-completed-needs-measurement-review";
    report["exitCode"] = 0;
    save();
    return 0;
}
