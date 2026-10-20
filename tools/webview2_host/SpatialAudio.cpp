#include "SpatialAudio.h"
#include "EmbeddedAudioExtension.h"
#include "Win32Util.h"
#include <wrl.h>
#include <wrl/client.h>
#include <fstream>
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
            if (FAILED(hr) || !collector) return E_NOINTERFACE;
            controller->put_Bounds(RECT{0,0,1,1});
            controller->put_IsVisible(FALSE);
            ComPtr<ICoreWebView2Settings> settings;
            if (FAILED(hr = collector->get_Settings(&settings))) return hr;
            if (FAILED(hr = settings->put_AreHostObjectsAllowed(FALSE))) return hr;
            if (FAILED(hr = settings->put_IsWebMessageEnabled(TRUE))) return hr;
            settings->put_AreDevToolsEnabled(FALSE);
            settings->put_AreDefaultContextMenusEnabled(FALSE);
            settings->put_AreDefaultScriptDialogsEnabled(FALSE);
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
            if (FAILED(hr)) return hr;
            hr = collector->add_FrameNavigationStarting(Callback<ICoreWebView2NavigationStartingEventHandler>(
                [](ICoreWebView2*, ICoreWebView2NavigationStartingEventArgs* args) -> HRESULT { return args->put_Cancel(TRUE); }).Get(), &token);
            if (FAILED(hr)) return hr;
            hr = collector->add_PermissionRequested(Callback<ICoreWebView2PermissionRequestedEventHandler>(
                [](ICoreWebView2*, ICoreWebView2PermissionRequestedEventArgs* args) -> HRESULT {
                    return args->put_State(COREWEBVIEW2_PERMISSION_STATE_DENY);
                }).Get(), &token);
            if (FAILED(hr)) return hr;
            hr = collector->add_NewWindowRequested(Callback<ICoreWebView2NewWindowRequestedEventHandler>(
                [](ICoreWebView2*, ICoreWebView2NewWindowRequestedEventArgs* args) -> HRESULT { return args->put_Handled(TRUE); }).Get(), &token);
            if (FAILED(hr)) return hr;
            ComPtr<ICoreWebView2_4> web4;
            if (FAILED(hr = collector.As(&web4))) return hr;
            hr = web4->add_DownloadStarting(Callback<ICoreWebView2DownloadStartingEventHandler>(
                [](ICoreWebView2*, ICoreWebView2DownloadStartingEventArgs* args) -> HRESULT { return args->put_Cancel(TRUE); }).Get(), &token);
            if (FAILED(hr)) return hr;
            hr = collector->add_ProcessFailed(Callback<ICoreWebView2ProcessFailedEventHandler>(
                [weak](ICoreWebView2*, ICoreWebView2ProcessFailedEventArgs*) -> HRESULT {
                    if (auto self = weak.lock()) self->Fail("Audio collector process failed");
                    return S_OK;
                }).Get(), &token);
            if (FAILED(hr)) return hr;
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
            if (FAILED(hr)) return hr;
            return collector->Navigate(kCollectorUrl.c_str());
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
    void SpatialAudio::Start(ICoreWebView2Environment* environment, HWND parent)
    {
        if (state->requested || state->failed || state->closed) return;
        state->requested = true;
        state->phase = "starting";
        state->startupDeadline = audio::ClockMs() + 15000;
        const std::weak_ptr<State> weak = state;
        const auto hr = environment->CreateCoreWebView2Controller(parent,
            Callback<ICoreWebView2CreateCoreWebView2ControllerCompletedHandler>(
                [weak](HRESULT result, ICoreWebView2Controller* controller) -> HRESULT {
                    const auto self = weak.lock();
                    if (!self || self->closed || self->failed) { if (controller) controller->Close(); return S_OK; }
                    if (FAILED(result) || !controller || FAILED(self->SetupCollector(controller)))
                        self->Fail("Could not initialize audio collector");
                    return S_OK;
                }).Get());
        if (FAILED(hr)) state->Fail("Could not create audio collector");
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
    json SpatialAudio::Status() const
    {
        return {{"route","webview2-tab-capture"},{"state",state->phase},{"error",state->error},
            {"fresh",audio::Fresh(state->expires,audio::ClockMs())},{"muted",state->muted}};
    }
}
