// Isolated feasibility test. Never links into or changes the production host.
#include <windows.h>
#include <wrl.h>
#include <WebView2.h>
#include <WebView2EnvironmentOptions.h>
#include <nlohmann/json.hpp>
#include "AudioMeter.h"
#include "SpatialAudio.h"

#include <chrono>
#include <filesystem>
#include <fstream>
#include <functional>
#include <iostream>
#include <memory>
#include <stdexcept>
#include <string>

using Microsoft::WRL::ComPtr;
using Microsoft::WRL::Callback;
using json = nlohmann::json;
namespace fs = std::filesystem;
namespace {
constexpr auto kExtensionId = L"nbligbkobhajkimckkfnadnghkimjcld";
constexpr auto kSource = L"https://osfui-tab-audio-source.test/source.html";
using Clock = std::chrono::steady_clock;

std::string Utf8(const std::wstring& s)
{
    const int size = WideCharToMultiByte(CP_UTF8, 0, s.data(), static_cast<int>(s.size()), nullptr, 0, nullptr, nullptr);
    std::string result(size, '\0');
    WideCharToMultiByte(CP_UTF8, 0, s.data(), static_cast<int>(s.size()), result.data(), size, nullptr, nullptr);
    return result;
}
void Check(HRESULT hr, const char* label)
{
    if (FAILED(hr)) {
        char code[20]{};
        snprintf(code, sizeof(code), "0x%08lX", static_cast<unsigned long>(hr));
        throw std::runtime_error(std::string(label) + ": " + code);
    }
}
void Pump()
{
    MSG message{};
    while (PeekMessageW(&message, nullptr, 0, 0, PM_REMOVE)) {
        if (message.message == WM_QUIT) throw std::runtime_error("Window loop quit");
        TranslateMessage(&message);
        DispatchMessageW(&message);
    }
    MsgWaitForMultipleObjectsEx(0, nullptr, 20, QS_ALLINPUT, MWMO_INPUTAVAILABLE);
}
void Until(const std::function<bool()>& predicate, unsigned timeoutMs = 15000)
{
    const auto deadline = Clock::now() + std::chrono::milliseconds(timeoutMs);
    while (!predicate()) {
        if (Clock::now() >= deadline) throw std::runtime_error("Async operation timed out");
        Pump();
    }
}
void Delay(unsigned ms)
{
    const auto end = Clock::now() + std::chrono::milliseconds(ms);
    Until([&] { return Clock::now() >= end; }, ms + 1000);
}
struct Completion {
    bool done{};
    HRESULT hr{E_PENDING};
    std::wstring text;
    void Set(HRESULT value) { hr = value; done = true; }
    void Wait(const char* label) { Until([&] { return done; }); Check(hr, label); }
};
struct View {
    HWND window{};
    ComPtr<ICoreWebView2Controller> controller;
    ComPtr<ICoreWebView2> web;
    json permissions = json::array();
    ~View()
    {
        if (controller) controller->Close();
        web.Reset(); controller.Reset();
        if (window) DestroyWindow(window);
    }
    void Create(ICoreWebView2Environment* env)
    {
        // Never shown or focused. Both controllers share this private environment.
        window = CreateWindowExW(WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE, L"STATIC",
            L"OSF UI private tab audio probe", WS_POPUP, 0, 0, 800, 600,
            nullptr, nullptr, GetModuleHandleW(nullptr), nullptr);
        if (!window) throw std::runtime_error("Create hidden parent");
        auto done = std::make_shared<Completion>();
        Check(env->CreateCoreWebView2Controller(window,
            Callback<ICoreWebView2CreateCoreWebView2ControllerCompletedHandler>(
                [this, done](HRESULT hr, ICoreWebView2Controller* result) -> HRESULT {
                    if (SUCCEEDED(hr) && result) controller = result;
                    done->Set(result ? hr : E_POINTER); return S_OK;
                }).Get()), "Create controller");
        done->Wait("Controller callback");
        Check(controller->get_CoreWebView2(&web), "Get WebView");
        Check(controller->put_Bounds({0, 0, 800, 600}), "Set bounds");
        Check(controller->put_IsVisible(TRUE), "Controller visible");
        ComPtr<ICoreWebView2Settings> settings;
        Check(web->get_Settings(&settings), "Settings");
        Check(settings->put_IsWebMessageEnabled(FALSE), "No web messages");
        Check(settings->put_AreHostObjectsAllowed(FALSE), "No host objects");
        Check(settings->put_AreDefaultScriptDialogsEnabled(FALSE), "No dialogs");
        EventRegistrationToken token{};
        Check(web->add_PermissionRequested(Callback<ICoreWebView2PermissionRequestedEventHandler>(
            [this](ICoreWebView2*, ICoreWebView2PermissionRequestedEventArgs* args) -> HRESULT {
                COREWEBVIEW2_PERMISSION_KIND kind{};
                LPWSTR uri{};
                args->get_PermissionKind(&kind);
                args->get_Uri(&uri);
                permissions.push_back({{"kind", static_cast<int>(kind)},
                    {"uri", uri ? Utf8(uri) : ""}, {"decision", "deny"}});
                CoTaskMemFree(uri);
                return args->put_State(COREWEBVIEW2_PERMISSION_STATE_DENY);
            }).Get(), &token), "Deny device permissions");
        ComPtr<ICoreWebView2_4> web4;
        Check(web.As(&web4), "Download control");
        Check(web4->add_DownloadStarting(Callback<ICoreWebView2DownloadStartingEventHandler>(
            [](ICoreWebView2*, ICoreWebView2DownloadStartingEventArgs* args) -> HRESULT {
                args->put_Handled(TRUE);
                return args->put_Cancel(TRUE);
            }).Get(), &token), "Block downloads");
    }
    void Navigate(const std::wstring& uri)
    {
        auto done = std::make_shared<Completion>();
        EventRegistrationToken token{};
        Check(web->add_NavigationCompleted(Callback<ICoreWebView2NavigationCompletedEventHandler>(
            [done](ICoreWebView2*, ICoreWebView2NavigationCompletedEventArgs* args) -> HRESULT {
                BOOL ok{}; args->get_IsSuccess(&ok);
                COREWEBVIEW2_WEB_ERROR_STATUS status{};
                args->get_WebErrorStatus(&status);
                done->text = std::to_wstring(static_cast<int>(status));
                done->Set(ok ? S_OK : E_FAIL); return S_OK;
            }).Get(), &token), "Navigation event");
        Check(web->Navigate(uri.c_str()), "Navigate");
        Until([&] { return done->done; });
        web->remove_NavigationCompleted(token);
        if (FAILED(done->hr))
            throw std::runtime_error("Navigation failed for " + Utf8(uri) +
                "; WebErrorStatus=" + Utf8(done->text));
    }
    json Script(const wchar_t* script)
    {
        auto done = std::make_shared<Completion>();
        Check(web->ExecuteScript(script, Callback<ICoreWebView2ExecuteScriptCompletedHandler>(
            [done](HRESULT hr, LPCWSTR result) -> HRESULT {
                if (result) done->text = result;
                done->Set(hr); return S_OK;
            }).Get()), "ExecuteScript");
        done->Wait("Script completion");
        auto value = json::parse(Utf8(done->text));
        return value.is_string() ? json::parse(value.get<std::string>()) : value;
    }
};
#include "ProductionRoute.inl"
}

int wmain(int argc, wchar_t** argv)
{
    fs::path root, output;
    bool allowlist{}, remoteVideo{}, nativeSourceMute{}, productionRoute{};
    for (int i = 1; i < argc; ++i) {
        const std::wstring arg(argv[i]);
        if (arg == L"--allowlist") allowlist = true;
        else if (arg == L"--remote-video") remoteVideo = true;
        else if (arg == L"--native-source-mute") nativeSourceMute = true;
        else if (arg == L"--production-route") productionRoute = true;
        else if (arg == L"--root" && i + 1 < argc) root = fs::absolute(argv[++i]);
        else if (arg == L"--output" && i + 1 < argc) output = fs::absolute(argv[++i]);
        else { std::cerr << "Unknown/missing option\n"; return 1; }
    }
    if (root.empty() || output.empty() || fs::exists(output)) {
        std::cerr << "Required: --root <prototype directory> --output <new directory> [--allowlist] [--remote-video] [--native-source-mute]\n";
        return 1;
    }
    fs::create_directories(output);
    json report{{"probe", "webview2-extension-tab-audio"}, {"allowlist", allowlist}, {"remoteVideo", remoteVideo},
        {"nativeSourceMute", nativeSourceMute},
        {"productionRoute", productionRoute},
        {"extensionId", Utf8(kExtensionId)}, {"hostPid", GetCurrentProcessId()},
        {"phases", json::array()}, {"result", "running"}};
    auto save = [&] {
        std::ofstream stream(output / "result.json");
        stream << report.dump(2) << '\n';
    };
    save();
    const auto com = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
    int code = 1;
    try {
        Check(com, "COM");
        ComPtr<ICoreWebView2Environment> environment;
        auto options = Microsoft::WRL::Make<CoreWebView2EnvironmentOptions>();
        ComPtr<ICoreWebView2EnvironmentOptions6> extensionOptions;
        Check(options.As(&extensionOptions), "Extension environment options");
        Check(extensionOptions->put_AreBrowserExtensionsEnabled(TRUE), "Enable extensions");
        std::wstring flags = L"--autoplay-policy=no-user-gesture-required "
            L"--disable-background-timer-throttling --disable-renderer-backgrounding "
            L"--disable-backgrounding-occluded-windows --disable-features=CalculateNativeWinOcclusion";
        if (!remoteVideo) flags += L" --proxy-server=http://osfui-egress.invalid:9 --proxy-bypass-list=<-loopback>";
        if (allowlist) flags += std::wstring(L" --allowlisted-extension-id=") + kExtensionId;
        report["browserArguments"] = Utf8(flags);
        Check(options->put_AdditionalBrowserArguments(flags.c_str()), "Arguments");
        auto done = std::make_shared<Completion>();
        Check(CreateCoreWebView2EnvironmentWithOptions(nullptr, (output / "profile").c_str(), options.Get(),
            Callback<ICoreWebView2CreateCoreWebView2EnvironmentCompletedHandler>(
                [&environment, done](HRESULT hr, ICoreWebView2Environment* env) -> HRESULT {
                    if (SUCCEEDED(hr) && env) environment = env;
                    done->Set(env ? hr : E_POINTER); return S_OK;
                }).Get()), "Create environment");
        done->Wait("Environment completion");
        LPWSTR version{};
        Check(environment->get_BrowserVersionString(&version), "Runtime version");
        report["runtime"] = Utf8(version); CoTaskMemFree(version);
        View source;
        View collector;
        source.Create(environment.Get());
        ComPtr<ICoreWebView2_8> sourceAudio;
        Check(source.web.As(&sourceAudio), "Source native audio control");
        BOOL originalSourceMuted{};
        Check(sourceAudio->get_IsMuted(&originalSourceMuted), "Original source native mute");
        report["originalSourceMuted"] = originalSourceMuted != FALSE;
        std::unique_ptr<osfui::webview2::SpatialAudio> productionAudio;
        if (productionRoute) {
            report["probe"] = "webview2-production-spatial-audio";
            report["productionLogs"] = json::array();
            productionAudio = std::make_unique<osfui::webview2::SpatialAudio>(source.web.Get(),
                [&](const std::string& message) { report["productionLogs"].push_back(message); });
            if (!productionAudio->Guarded()) throw std::runtime_error("Production native source mute guard failed");
        }
        UINT32 browserPid{};
        Check(source.web->get_BrowserProcessId(&browserPid), "Browser PID");
        report["browserPid"] = browserPid;
        ComPtr<ICoreWebView2_13> web13;
        ComPtr<ICoreWebView2Profile> profile;
        ComPtr<ICoreWebView2Profile7> profile7;
        Check(source.web.As(&web13), "Profile accessor");
        Check(web13->get_Profile(&profile), "Profile");
        Check(profile.As(&profile7), "Extension profile");
        done = std::make_shared<Completion>();
        const auto extensionPath = productionRoute
            ? osfui::webview2::PrepareAudioExtension(output / "profile") : root / "extension";
        Check(profile7->AddBrowserExtension(extensionPath.c_str(),
            Callback<ICoreWebView2ProfileAddBrowserExtensionCompletedHandler>(
                [done](HRESULT hr, ICoreWebView2BrowserExtension* extension) -> HRESULT {
                    if (SUCCEEDED(hr) && extension) {
                        LPWSTR id{};
                        hr = extension->get_Id(&id);
                        if (id) { done->text = id; CoTaskMemFree(id); }
                    }
                    done->Set(hr); return S_OK;
                }).Get()), "Add extension");
        done->Wait("Extension completion");
        report["installedExtensionId"] = Utf8(done->text);
        if (done->text != kExtensionId) throw std::runtime_error("Extension ID differs from allowlist ID");
        save();
        ComPtr<ICoreWebView2_3> web3;
        Check(source.web.As(&web3), "Virtual host");
        Check(web3->SetVirtualHostNameToFolderMapping(L"osfui-tab-audio-source.test",
            (root / "source").c_str(), COREWEBVIEW2_HOST_RESOURCE_ACCESS_KIND_DENY_CORS), "Map fixture");
        if (productionRoute) {
            code = RunProductionRoute(source, environment.Get(), productionAudio, report, save);
        } else {
        source.Navigate(kSource);
        if (remoteVideo) source.Script(L"sourceProbe.prepareVideo(); null");
        collector.Create(environment.Get());
        const std::wstring collectorUrl = std::wstring(L"chrome-extension://") + kExtensionId +
            L"/collector.html";
        collector.Navigate(collectorUrl);
        Delay(500);
        report["collectorInitial"] = collector.Script(L"JSON.stringify(window.probe)");
        save();

        AudioMeter meter;
        if (!meter.Start()) throw std::runtime_error("Endpoint meter start: " + meter.Snapshot().dump());
        const auto sourceState = [&] {
            auto state = source.Script(L"JSON.stringify(sourceProbe.snapshot())");
            BOOL muted{};
            Check(sourceAudio->get_IsMuted(&muted), "Source native mute readback");
            state["nativeMuted"] = muted != FALSE;
            return state;
        };
        auto measure = [&](const char* name, unsigned duration = 2000) {
            Delay(500);
            if (!meter.Reset()) throw std::runtime_error("Endpoint meter reset: " + meter.Snapshot().dump());
            Delay(duration);
            report["phases"].push_back({{"name", name}, {"endpoint", meter.Snapshot()},
                {"collector", collector.Script(L"JSON.stringify(window.probe)")},
                {"source", sourceState()}});
            save();
        };
        measure("quiet-background", 1000);
        source.Script(L"sourceProbe.start(); null");
        if (remoteVideo) Delay(4000); // Let the remote clip reach an audible section.
        measure("original-browser-audio");
        collector.Script(L"startCapture('silent'); null");
        const auto captureDeadline = Clock::now() + std::chrono::seconds(10);
        do {
            Delay(100);
            report["captureAttempt"] = collector.Script(L"JSON.stringify(window.probe)");
            const auto state = report["captureAttempt"].value("state", "");
            if (state == "capturing" || state == "error") break;
        } while (Clock::now() < captureDeadline);
        report["sourcePermissions"] = source.permissions;
        report["collectorPermissions"] = collector.permissions;
        save();
        const auto capture = report["captureAttempt"];
        // Capture API failures are valid negative evidence; do not mask them with fallback audio.
        if (capture.value("state", "") != "capturing") {
            report["result"] = "capture-rejected";
            code = 2;
        } else {
            measure("capture-without-output");
            collector.Script(L"setAudioMode('center'); null");
            measure("spatial-center");
            collector.Script(L"setAudioMode('left'); null");
            measure("spatial-left");
            collector.Script(L"setAudioMode('right'); null");
            measure("spatial-right");
            collector.Script(L"setAudioMode('far'); null");
            measure("spatial-far");
            if (nativeSourceMute) {
                Check(sourceAudio->put_IsMuted(TRUE), "Mute capture source natively");
                collector.Script(L"setAudioMode('silent'); null");
                measure("native-source-muted-capture-silent");
                collector.Script(L"setAudioMode('center'); null");
                measure("native-source-muted-capture-center");
                collector.Script(L"stopCapture(); null");
                measure("native-source-muted-capture-stopped");
                collector.Script(L"startCapture('center'); null");
                const auto mutedCaptureDeadline = Clock::now() + std::chrono::seconds(10);
                do {
                    Delay(100);
                    report["mutedCaptureAttempt"] = collector.Script(L"JSON.stringify(window.probe)");
                    const auto state = report["mutedCaptureAttempt"].value("state", "");
                    if (state == "capturing" || state == "error") break;
                } while (Clock::now() < mutedCaptureDeadline);
                measure("native-source-muted-capture-restarted");
                collector.Script(L"stopCapture(); null");
                measure("native-source-muted-restarted-capture-stopped");
                Check(sourceAudio->put_IsMuted(originalSourceMuted), "Restore source native mute");
            }
            collector.Script(L"stopCapture(); null");
            measure("capture-stopped-original-restored");
            report["result"] = "capture-completed-needs-measurement-review";
            code = 0;
        }
        collector.Script(L"stopCapture(); null");
        source.Script(L"sourceProbe.stop(); null");
        Check(sourceAudio->put_IsMuted(originalSourceMuted), "Restore source native mute at cleanup");
        Delay(300);
        report["finalSource"] = sourceState();
        meter.Stop();
        report["exitCode"] = code;
        save();
        }
    } catch (const std::exception& error) {
        code = 1;
        report["result"] = "infrastructure-error";
        report["error"] = error.what();
        report["exitCode"] = 1;
        save();
        std::cerr << error.what() << '\n';
    }
    if (SUCCEEDED(com)) CoUninitialize();
    std::cout << Utf8((output / "result.json").wstring()) << '\n';
    return code;
}
