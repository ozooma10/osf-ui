#include "SpatialAudio.h"
#include "EmbeddedAudioExtension.h"
#include "Win32Util.h"
#include <wrl.h>
#include <wrl/client.h>
#include <fstream>
#include <format>
#include <iterator>
#include <stdexcept>

using Microsoft::WRL::ComPtr;
using Microsoft::WRL::Callback;
using nlohmann::json;
namespace osfui::webview2
{
    namespace audio = osfui::browser::audio;
    using osfui::win32::ToWide;
    using osfui::win32::ToUtf8;
    namespace
    {
        const std::wstring kCollectorUrl = std::wstring(L"chrome-extension://") + kAudioExtensionId + L"/collector.html";
        std::uint64_t UtcMs()
        {
            FILETIME ft{};
            GetSystemTimeAsFileTime(&ft);
            ULARGE_INTEGER time{.LowPart=ft.dwLowDateTime,.HighPart=ft.dwHighDateTime};
            return time.QuadPart / 10000 - 11644473600000ULL;
        }
    }

    std::filesystem::path PrepareAudioExtension(const std::filesystem::path& userData)
    {
        const auto path = userData / L"OSFUIAudio" / EmbeddedAudioExtensionContentId();
        std::filesystem::create_directories(path);
        for (const auto& asset : GetEmbeddedAudioExtension()) {
            const auto file = path / asset.name;
            if (!std::filesystem::exists(file)) {
                std::ofstream output(file, std::ios::binary);
                output.write(asset.bytes.data(), static_cast<std::streamsize>(asset.bytes.size()));
                if (!output) throw std::runtime_error("Could not extract browser audio extension");
            }
            std::ifstream input(file, std::ios::binary);
            const std::string bytes{std::istreambuf_iterator<char>(input), {}};
            if (!input || bytes != asset.bytes) throw std::runtime_error("Browser audio extension cache differs from embedded assets");
        }
        return path;
    }

    struct SpatialAudio::State : std::enable_shared_from_this<State>
    {
        ComPtr<ICoreWebView2> source, collector;
        ComPtr<ICoreWebView2_8> sourceAudio;
        ComPtr<ICoreWebView2Controller> controller;
        Log log;
        bool closed{}, guarded{}, requested{}, ready{}, capturing{}, failed{}, muted{}, sceneDirty{}, loading{}, commandPending{};
        std::uint64_t startupDeadline{}, expires{}, nextTick{}, commandDeadline{};
        json scene;
        std::string phase{"waiting"}, error;

        void Fail(const std::string& reason)
        {
            if (closed) return;
            if (sourceAudio) sourceAudio->put_IsMuted(TRUE);
            capturing = false;
            failed = true;
            phase = "failed";
            if (error != reason) { error = reason; log(reason); }
            // Retain the stream until source mute has been requested. A stopped
            // tabCapture stream otherwise restores ordinary browser output.
            if (collector) collector->ExecuteScript(L"window.osfAudio?.stop()", nullptr);
        }
        HRESULT FailResult(std::string_view operation, HRESULT result)
        {
            Fail(std::format("Audio collector {} failed (HRESULT 0x{:08X})",
                operation, static_cast<std::uint32_t>(result)));
            return result;
        }
        void Close()
        {
            if (closed) return;
            if (sourceAudio) sourceAudio->put_IsMuted(TRUE);
            closed = true;
            if (controller) controller->Close();
            collector.Reset(); controller.Reset(); source.Reset(); sourceAudio.Reset();
        }
        bool Execute(const std::wstring& script)
        {
            if (closed || failed || !collector || commandPending) return false;
            commandPending = true;
            commandDeadline = audio::ClockMs() + 2000;
            const auto weak = weak_from_this();
            const auto hr = collector->ExecuteScript(script.c_str(), Callback<ICoreWebView2ExecuteScriptCompletedHandler>(
                [weak](HRESULT result, LPCWSTR) -> HRESULT {
                    if (const auto self = weak.lock(); self && !self->closed) {
                        self->commandPending = false;
                        if (FAILED(result)) self->Fail("Audio collector script execution failed");
                    }
                    return S_OK;
                }).Get());
            if (FAILED(hr)) { commandPending = false; Fail("Audio collector script dispatch failed"); return false; }
            return true;
        }
        void PushScene()
        {
            if (!capturing || failed || !collector) return;
            const auto now = audio::ClockMs();
            auto value = scene;
            if (!audio::Fresh(expires, now) || !value.is_object()) {
                value = audio::Encode(OSFUI::API::Surfaces::AudioScene{});
                value["gain"] = 0;
                value["deadlineUtcMs"] = UtcMs();
            } else value["deadlineUtcMs"] = UtcMs() + expires - now;
            value["muted"] = muted;
            sceneDirty = !Execute(L"window.osfAudio.setScene(" + ToWide(value.dump()) + L")");
        }
        HRESULT SetupCollector(ICoreWebView2Controller* value)
        {
            controller = value;
            HRESULT hr = controller->get_CoreWebView2(&collector);
            if (FAILED(hr)) return FailResult("get_CoreWebView2", hr);
            if (!collector) return FailResult("get_CoreWebView2 returned null", E_POINTER);
            if (FAILED(hr = controller->put_Bounds(RECT{0,0,1,1}))) return FailResult("put_Bounds", hr);
            if (FAILED(hr = controller->put_IsVisible(FALSE))) return FailResult("put_IsVisible", hr);
            ComPtr<ICoreWebView2Settings> settings;
            if (FAILED(hr = collector->get_Settings(&settings))) return FailResult("get_Settings", hr);
            if (!settings) return FailResult("get_Settings returned null", E_POINTER);
            if (FAILED(hr = settings->put_AreHostObjectsAllowed(FALSE))) return FailResult("put_AreHostObjectsAllowed", hr);
            if (FAILED(hr = settings->put_IsWebMessageEnabled(TRUE))) return FailResult("put_IsWebMessageEnabled", hr);
            if (FAILED(hr = settings->put_AreDevToolsEnabled(FALSE))) return FailResult("put_AreDevToolsEnabled", hr);
            if (FAILED(hr = settings->put_AreDefaultContextMenusEnabled(FALSE))) return FailResult("put_AreDefaultContextMenusEnabled", hr);
            if (FAILED(hr = settings->put_AreDefaultScriptDialogsEnabled(FALSE))) return FailResult("put_AreDefaultScriptDialogsEnabled", hr);
            const auto weak = weak_from_this();
            EventRegistrationToken token{};
            hr = collector->add_NavigationStarting(Callback<ICoreWebView2NavigationStartingEventHandler>(
                [weak](ICoreWebView2*, ICoreWebView2NavigationStartingEventArgs* args) -> HRESULT {
                    LPWSTR raw{};
                    const auto result = args->get_Uri(&raw);
                    const bool allowed = SUCCEEDED(result) && raw && raw == kCollectorUrl;
                    CoTaskMemFree(raw);
                    if (!allowed) { args->put_Cancel(TRUE); if (auto self=weak.lock()) self->Fail("Audio collector navigation rejected"); }
                    return S_OK;
                }).Get(), &token);
            if (FAILED(hr)) return FailResult("add_NavigationStarting", hr);
            hr = collector->add_FrameNavigationStarting(Callback<ICoreWebView2NavigationStartingEventHandler>(
                [](ICoreWebView2*, ICoreWebView2NavigationStartingEventArgs* args) -> HRESULT { return args->put_Cancel(TRUE); }).Get(), &token);
            if (FAILED(hr)) return FailResult("add_FrameNavigationStarting", hr);
            hr = collector->add_PermissionRequested(Callback<ICoreWebView2PermissionRequestedEventHandler>(
                [](ICoreWebView2*, ICoreWebView2PermissionRequestedEventArgs* args) -> HRESULT {
                    return args->put_State(COREWEBVIEW2_PERMISSION_STATE_DENY);
                }).Get(), &token);
            if (FAILED(hr)) return FailResult("add_PermissionRequested", hr);
            hr = collector->add_NewWindowRequested(Callback<ICoreWebView2NewWindowRequestedEventHandler>(
                [](ICoreWebView2*, ICoreWebView2NewWindowRequestedEventArgs* args) -> HRESULT { return args->put_Handled(TRUE); }).Get(), &token);
            if (FAILED(hr)) return FailResult("add_NewWindowRequested", hr);
            ComPtr<ICoreWebView2_4> web4;
            if (FAILED(hr = collector.As(&web4))) return FailResult("query ICoreWebView2_4", hr);
            hr = web4->add_DownloadStarting(Callback<ICoreWebView2DownloadStartingEventHandler>(
                [](ICoreWebView2*, ICoreWebView2DownloadStartingEventArgs* args) -> HRESULT { return args->put_Cancel(TRUE); }).Get(), &token);
            if (FAILED(hr)) return FailResult("add_DownloadStarting", hr);
            hr = collector->add_ProcessFailed(Callback<ICoreWebView2ProcessFailedEventHandler>(
                [weak](ICoreWebView2*, ICoreWebView2ProcessFailedEventArgs*) -> HRESULT {
                    if (auto self = weak.lock()) self->Fail("Audio collector process failed");
                    return S_OK;
                }).Get(), &token);
            if (FAILED(hr)) return FailResult("add_ProcessFailed", hr);
            hr = collector->add_WebMessageReceived(Callback<ICoreWebView2WebMessageReceivedEventHandler>(
                [weak](ICoreWebView2*, ICoreWebView2WebMessageReceivedEventArgs* args) -> HRESULT {
                    const auto self = weak.lock();
                    if (!self || self->closed || self->failed) return S_OK;
                    LPWSTR origin{}, raw{};
                    const auto originHr = args->get_Source(&origin);
                    const bool trusted = SUCCEEDED(originHr) && origin && origin == kCollectorUrl;
                    CoTaskMemFree(origin);
                    if (!trusted) { self->Fail("Audio collector message origin rejected"); return S_OK; }
                    if (FAILED(args->TryGetWebMessageAsString(&raw)) || !raw) return S_OK;
                    const auto length = wcsnlen_s(raw, 4097);
                    const auto message = length <= 4096 ? ToUtf8(raw) : std::string{};
                    CoTaskMemFree(raw);
                    const auto data = json::parse(message, nullptr, false);
                    if (!data.is_object() || data.value("kind", "") != "osfui.audio") return S_OK;
                    const auto state = data.value("state", "");
                    if (state == "ready") { self->ready = true; self->startupDeadline = 0; }
                    else if (state == "capturing") {
                        self->capturing = true;
                        self->phase = "capturing";
                        self->PushScene();
                    } else if (state == "failed" || state == "stopped") self->Fail(data.value("error", "Audio capture stopped"));
                    else if (state == "silent") self->phase = "silent";
                    return S_OK;
                }).Get(), &token);
            if (FAILED(hr)) return FailResult("add_WebMessageReceived", hr);
            hr = collector->Navigate(kCollectorUrl.c_str());
            return FAILED(hr) ? FailResult("Navigate extension collector", hr) : hr;
        }
        void Tick()
        {
            if (closed || failed || !requested) return;
            const auto now = audio::ClockMs();
            if (now < nextTick) return;
            nextTick = now + 50;
            if (commandPending && now >= commandDeadline) { Fail("Audio collector is unresponsive"); return; }
            if (!capturing && startupDeadline && now >= startupDeadline) { Fail("Audio capture startup timed out"); return; }
            if (ready && !loading && phase == "starting") {
                LPWSTR raw{};
                const auto hr = source->get_Source(&raw);
                std::wstring url = SUCCEEDED(hr) && raw ? raw : L"";
                CoTaskMemFree(raw);
                if (!url.empty() && url != L"about:blank") {
                    if (Execute(L"window.osfAudio.configure(" + ToWide(json{{"sourceUrl",ToUtf8(url)}}.dump()) + L")")) {
                        phase = "capturing-pending";
                        startupDeadline = now + 15000;
                    }
                }
            }
            if (capturing && (sceneDirty || (expires && !audio::Fresh(expires, now)))) {
                if (!audio::Fresh(expires, now)) expires = 0;
                PushScene();
            }
        }
    };

    SpatialAudio::SpatialAudio(ICoreWebView2* source, Log log) : state(std::make_shared<State>())
    {
        state->source = source;
        state->log = std::move(log);
        // IsMuted gates the source's ordinary output, not its tabCapture stream.
        // Never unmute this controller, even during startup, failure or teardown;
        // user mute belongs to the collector graph. This also guards a collector crash.
        state->guarded = SUCCEEDED(state->source.As(&state->sourceAudio)) &&
            SUCCEEDED(state->sourceAudio->put_IsMuted(TRUE));
        if (!state->guarded) state->Fail("Native audio mute guard unavailable");
    }
    SpatialAudio::~SpatialAudio() { state->Close(); }
    bool SpatialAudio::Guarded() const { return state->guarded; }
    void SpatialAudio::Start(ICoreWebView2Environment* environment, HWND parent, bool composition)
    {
        if (state->requested || state->failed || state->closed) return;
        if (!environment) { state->FailResult("Start environment is null", E_POINTER); return; }
        if (!::IsWindow(parent)) { state->FailResult("Start parent HWND is invalid", E_INVALIDARG); return; }
        state->requested = true;
        state->phase = "starting";
        state->startupDeadline = audio::ClockMs() + 15000;
        const std::weak_ptr<State> weak = state;
        const auto completed = [weak, composition](HRESULT result, ICoreWebView2Controller* controller) -> HRESULT {
            const auto self = weak.lock();
            if (!self || self->closed || self->failed) { if (controller) controller->Close(); return S_OK; }
            if (FAILED(result)) self->FailResult(composition ?
                "CreateCoreWebView2CompositionController completion" : "CreateCoreWebView2Controller completion", result);
            else if (!controller) self->FailResult("controller creation returned null", E_POINTER);
            else self->SetupCollector(controller);
            return S_OK;
        };
        HRESULT hr{};
        if (composition) {
            // Match the source controllers' hosting mode in this environment.
            // The hidden collector needs no root visual or input forwarding.
            ComPtr<ICoreWebView2Environment3> environment3;
            hr = environment->QueryInterface(IID_PPV_ARGS(&environment3));
            if (FAILED(hr)) { state->FailResult("query ICoreWebView2Environment3", hr); return; }
            hr = environment3->CreateCoreWebView2CompositionController(parent,
                Callback<ICoreWebView2CreateCoreWebView2CompositionControllerCompletedHandler>(
                    [weak, completed](HRESULT result, ICoreWebView2CompositionController* value) -> HRESULT {
                        if (FAILED(result) || !value) return completed(result, nullptr);
                        ComPtr<ICoreWebView2Controller> controller;
                        const auto queryHr = value->QueryInterface(IID_PPV_ARGS(&controller));
                        if (FAILED(queryHr)) {
                            if (const auto self = weak.lock()) self->FailResult("query collector ICoreWebView2Controller", queryHr);
                            return S_OK;
                        }
                        return completed(result, controller.Get());
                    }).Get());
        } else {
            hr = environment->CreateCoreWebView2Controller(parent,
                Callback<ICoreWebView2CreateCoreWebView2ControllerCompletedHandler>(completed).Get());
        }
        if (FAILED(hr)) state->FailResult(composition ?
            "CreateCoreWebView2CompositionController dispatch" : "CreateCoreWebView2Controller dispatch", hr);
    }
    void SpatialAudio::Update(const json& scene, std::uint64_t expires)
    {
        const auto decoded = audio::Decode(scene);
        state->scene = decoded ? audio::Encode(*decoded) : json{};
        state->expires = decoded && audio::Fresh(expires, audio::ClockMs()) ? expires : 0;
        state->sceneDirty = true;
    }
    void SpatialAudio::Tick() { state->Tick(); }
    void SpatialAudio::Fail(const std::string& reason) { state->Fail(reason); }
    void SpatialAudio::SetMuted(bool muted) { state->muted = muted; state->sceneDirty = true; state->PushScene(); }
    void SpatialAudio::SetLoading(bool loading) { state->loading = loading; }
    bool SpatialAudio::Muted() const { return state->muted; }
    json SpatialAudio::Status(bool includeSpatial) const
    {
        const bool fresh = audio::Fresh(state->expires,audio::ClockMs());
        json status{{"route","webview2-tab-capture"},{"state",state->phase},{"error",state->error},
            {"fresh",fresh},{"muted",state->muted}};
        if (!includeSpatial) return status;
        // Report the validated native scene supplied to the collector. This is
        // pose/routing evidence, not a measurement of the browser's audio output.
        const auto decoded = audio::Decode(state->scene);
        const auto scene = decoded.value_or(OSFUI::API::Surfaces::AudioScene{});
        auto emitters = json::array();
        for (unsigned i = 0; i < scene.count; ++i) {
            const auto& emitter = scene.emitters[i];
            emitters.push_back({{"id",emitter.id},
                {"distance",std::sqrt(audio::DistanceSquared(scene.position,emitter.position))},
                {"position",audio::Vector(emitter.position)}});
        }
        status["spatial"] = {{"fresh",fresh},
            {"enabled",fresh && state->capturing && !state->failed && !state->closed &&
                scene.count > 0 && !state->muted && scene.gain > 0},
            {"muted",state->muted},{"gain",scene.gain},
            {"listener",audio::Vector(scene.position)},{"front",audio::Vector(scene.front)},
            {"up",audio::Vector(scene.up)},{"emitters",std::move(emitters)},{"emitterCount",scene.count}};
        return status;
    }
}
