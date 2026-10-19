// Disposable SFSE consumer. Exercises the shipped public API without adding
// test hooks to OSFUI.dll. The process owns this plugin for its entire lifetime.
#include <RE/Starfield.h>
#include <SFSE/SFSE.h>
#include <OSFUI.h>
#include <vendor/OSFSettings.h>
#include <vendor/OSFSettings_Providers.h>
#include <RE/E/Events.h>
#include <RE/M/MenuControls.h>
#include <RE/C/CreationRenderer.h>
#include "../../src/Compat/V1/LegacyABI.h"
#include <nlohmann/json.hpp>
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <Windows.h>
#include <d3d12.h>
#undef ERROR
#include <filesystem>
#include <fstream>
#include <mutex>
#include <thread>

namespace {
using json = nlohmann::json;
namespace fs = std::filesystem;
OSFUI::API::Client api;
OSFSettings::API::Client settings;
OSFSettings::API::Providers::Client providers;
OSFUI::Compat::V1::IOSFUIBridge* legacy{};
std::mutex mutex;
json observations = {{"pages", json::object()}, {"pointer", json::object()}, {"lifecycle", json::object()}};
fs::path directory;
std::string session;
fs::path Utf8Path(const std::string& value) { return fs::path(std::u8string(value.begin(), value.end())); }

void Write(const fs::path& path, const json& value) {
    const auto temporary = path.wstring() + L".tmp";
    { std::ofstream stream(temporary, std::ios::binary); stream << value.dump(); }
    // Windows can report either sharing violation or access denied when a
    // reader briefly holds the destination without delete sharing. Retry this
    // publication (never the command mutation) for a bounded half second.
    for (int attempt = 0; ; ++attempt) {
        if (MoveFileExW(temporary.c_str(), path.c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)) return;
        const auto error = GetLastError();
        if ((error != ERROR_SHARING_VIOLATION && error != ERROR_ACCESS_DENIED) || attempt == 99)
            throw std::runtime_error("Could not publish consumer evidence: " + std::to_string(error));
        Sleep(5);
    }
}
// Production Settings keeps its normal Documents path. A separate USVFS
// controller maps only this owned test session's storage before plugin Load.
// No shipped DLL, API or settings implementation is patched.
bool PreparePrivateStorage() {
    json config;
    { std::ifstream stream("Data/SFSE/Plugins/OSFUIBackendTest.json");
      if (!stream) return true;
      stream >> config; }
    if (!config.contains("storage")) return true;
    const auto output = Utf8Path(config.at("directory").get<std::string>());
    const auto token = config.at("session").get<std::string>();
    const auto& storage = config.at("storage");
    FILETIME created{}, exited{}, kernel{}, user{};
    if (!GetProcessTimes(GetCurrentProcess(), &created, &exited, &kernel, &user)) return false;
    const auto creation = (static_cast<std::uint64_t>(created.dwHighDateTime) << 32) | created.dwLowDateTime;
    Write(output / "storage-ready.json", {{"session", token}, {"pid", GetCurrentProcessId()},
        {"processStartFileTime", std::to_string(creation)}});
    const auto deadline = GetTickCount64() + 30000;
    while (GetTickCount64() < deadline) {
        json receipt = json::object();
        { std::ifstream stream(output / "storage-mapped.json"); if (stream) stream >> receipt; }
        if (receipt.value("session", "") == token) {
            if (!receipt.value("accepted", false)) return false;
            for (const auto& mapping : storage.at("mappings")) {
                json marker;
                std::ifstream(Utf8Path(mapping.at("destination").get<std::string>()) / ".osf-ui-storage.json") >> marker;
                if (marker.value("session", "") != token) return false;
            }
            Write(output / "storage-verified.json", {{"session", token}, {"pid", GetCurrentProcessId()}, {"verified", true}});
            return true;
        }
        Sleep(50);
    }
    return false;
}
void Report(const char*, const char* payload, const char* view, void*) noexcept {
    try {
        auto value = json::parse(payload);
        value["reportedTickMs"] = GetTickCount64();
        std::scoped_lock lock(mutex);
        observations["pages"][view] = std::move(value);
    } catch (...) {}
}
void Close(const char*, const char*, const char* view, void*) noexcept { api.RequestMenu(view, false); }
void Echo(const OSFUI::API::Request& request, void*) noexcept { request.Reply(request.payloadJson); }
void Reject(const OSFUI::API::Request& request, void*) noexcept { request.Reject("fixture-rejection", "Expected test rejection"); }
void LegacyEcho(const OSFUI::Compat::V1::Request& request, void*) noexcept { request.Respond("backend.reply", request.payloadJson); }
void Pointer(const char*, OSFUI::API::RelativePointerPhase phase, float dx, float dy, float, void*) noexcept {
    std::scoped_lock lock(mutex);
    auto& p = observations["pointer"];
    const auto key = std::to_string(static_cast<int>(phase));
    p[key] = p.value(key, 0) + 1;
    p["distance"] = p.value("distance", 0.0) + std::abs(dx) + std::abs(dy);
}
void Lifecycle(const char* view, OSFUI::API::ViewLifecyclePhase phase, void*) noexcept {
    std::scoped_lock lock(mutex);
    auto& v = observations["lifecycle"][view];
    if (!v.is_object()) v = json::object();
    const auto key = std::to_string(static_cast<int>(phase));
    v[key] = v.value(key, 0) + 1;
}
void LegacySetting(const char* mod, const char* key, const char* value, void*) noexcept {
    try {
        std::scoped_lock lock(mutex);
        observations["legacySettings"][mod][key ? key : "refresh"] = value ? json::parse(value) : json(nullptr);
    } catch (...) {}
}
void ObserveHotkey(const char* mod, const char* key, void* context) noexcept {
    std::scoped_lock lock(mutex);
    auto& counts = observations[context ? "providerHotkeys" : "legacyHotkeys"];
    if (!counts.is_object()) counts = json::object();
    const auto name = std::string(mod) + "/" + key;
    counts[name] = counts.value(name, 0) + 1;
}
void Run() {
    std::string last;
    for (;;) {
        try {
            json command = json::object();
            { std::ifstream stream(directory / "command.json"); if (stream) stream >> command; }
            const auto id = command.value("id", "");
            if (!id.empty() && id != last && command.value("session", "") == session) {
                last = id;
                bool accepted = false;
                const auto action = command.value("action", "");
                const auto view = command.value("view", "osfui-backend-test/panel");
                // Never replay a timed-out mutation from an earlier scenario.
                if (command.value("deadline", 0ull) >= GetTickCount64()) {
                    if (action == "menu") accepted = api.RequestMenu(view.c_str(), command.value("open", false));
                    if (action == "state") accepted = api.SetState("osfui-backend-test", "value", command.at("value").dump().c_str());
                    if (action == "event") accepted = api.EmitEvent(view.c_str(), command.at("name").get<std::string>().c_str(), command.at("value").dump().c_str());
                    if (action == "graphics") {
                        // Read the same typed renderer interface used by the production
                        // compositor. No game mutation or rendering hook is installed.
                        auto* renderer = RE::CreationRendererPrivate::Renderer::GetSingleton();
                        auto* unknown = renderer ? reinterpret_cast<IUnknown*>(renderer->GetDevice()) : nullptr;
                        ID3D12Device* device{};
                        if (unknown && SUCCEEDED(unknown->QueryInterface(IID_PPV_ARGS(&device)))) {
                            const auto luid = device->GetAdapterLuid();
                            device->Release();
                            std::scoped_lock lock(mutex);
                            observations["graphics"] = {{"adapterLuidHigh", luid.HighPart}, {"adapterLuidLow", luid.LowPart}};
                            accepted = true;
                        }
                    }
                    if (legacy && action == "legacyMenu") accepted = legacy->RequestMenu("osfui-backend-test/legacy", command.value("open", false));
                    if (legacy && action == "legacyState") accepted = legacy->SetViewState("osfui-backend-test", "legacy", command.at("value").dump().c_str());
                    if (legacy && action == "legacyEvent") accepted = legacy->SendToWeb("osfui-backend-test/legacy", "backend.event", command.at("value").dump().c_str());
                    if (legacy && action == "legacySubscribe") accepted = legacy->SubscribeSettings(command.at("mod").get<std::string>().c_str(), LegacySetting, nullptr) != 0;
                    if (legacy && action == "observeHotkey") {
                        const auto mod = command.at("mod").get<std::string>();
                        const auto key = command.at("key").get<std::string>();
                        OSFSettings::API::Subscription token{};
                        accepted = providers.Init() && providers.SubscribeKey(mod.c_str(), key.c_str(), ObserveHotkey,
                            &providers, &token) == OSFSettings::API::Status::Ok;
                        accepted = accepted && legacy->SubscribeHotkey(mod.c_str(), key.c_str(), ObserveHotkey, nullptr) != 0;
                    }
                    if (action == "settingsBool") {
                        const auto mod = command.at("mod").get<std::string>();
                        const auto key = command.at("key").get<std::string>();
                        if (command.contains("value"))
                            accepted = settings.SetBool(mod.c_str(), key.c_str(), command.at("value").get<bool>()) == OSFSettings::API::Status::Ok;
                        else accepted = true;
                        bool value{};
                        accepted = accepted && settings.GetBool(mod.c_str(), key.c_str(), &value) == OSFSettings::API::Status::Ok;
                        std::scoped_lock lock(mutex);
                        observations["settings"] = {{"mod", mod}, {"key", key}, {"value", value}, {"command", id}};
                    }
                    if (action == "settingsKey") {
                        const auto mod = command.at("mod").get<std::string>();
                        const auto key = command.at("key").get<std::string>();
                        if (command.contains("value"))
                            accepted = settings.SetKey(mod.c_str(), key.c_str(), command.at("value").get<std::uint32_t>()) == OSFSettings::API::Status::Ok;
                        else accepted = true;
                        std::uint32_t value{};
                        accepted = accepted && settings.GetKey(mod.c_str(), key.c_str(), &value) == OSFSettings::API::Status::Ok;
                        std::scoped_lock lock(mutex);
                        observations["settings"] = {{"mod", mod}, {"key", key}, {"value", value}, {"command", id}};
                    }
                    if (action == "settingsFloat") {
                        const auto mod = command.at("mod").get<std::string>();
                        const auto key = command.at("key").get<std::string>();
                        if (command.contains("value"))
                            accepted = settings.SetFloat(mod.c_str(), key.c_str(), command.at("value").get<double>()) == OSFSettings::API::Status::Ok;
                        else accepted = true;
                        double value{};
                        accepted = accepted && settings.GetFloat(mod.c_str(), key.c_str(), &value) == OSFSettings::API::Status::Ok;
                        std::scoped_lock lock(mutex);
                        observations["settings"] = {{"mod", mod}, {"key", key}, {"value", value}, {"command", id}};
                    }
                }
                Write(directory / "reply.json", {{"id", id}, {"session", session}, {"accepted", accepted}});
            }
            json snapshot;
            { std::scoped_lock lock(mutex); snapshot = observations; }
            snapshot["session"] = session; snapshot["pid"] = GetCurrentProcessId();
            snapshot["tickMs"] = GetTickCount64(); snapshot["ready"] = api.IsReady();
            Write(directory / "consumer.json", snapshot);
        } catch (const std::exception& error) {
            std::scoped_lock lock(mutex); observations["error"] = error.what();
        }
        Sleep(50);
    }
}
class LoadObserver final : public RE::BSTEventSink<RE::TESLoadGameEvent> {
    RE::BSEventNotifyControl ProcessEvent(const RE::TESLoadGameEvent&, RE::BSTEventSource<RE::TESLoadGameEvent>*) override {
        std::scoped_lock lock(mutex);
        observations["loadGeneration"] = observations.value("loadGeneration", 0) + 1;
        return RE::BSEventNotifyControl::kContinue;
    }
};
void OnMessage(SFSE::MessagingInterface::Message* message) {
    if (message && message->type == SFSE::MessagingInterface::kPostDataLoad) {
        // Test-only startup comparison, before the disposable save is loaded.
        // No remap or ControlMap file is written and no production DLL changes.
        try {
            json config; std::ifstream("Data/SFSE/Plugins/OSFUIBackendTest.json") >> config;
            if (config.value("diagnosticDisableQuickSaveLoad", false)) {
                auto* controls = RE::MenuControls::GetSingleton();
                const bool disabled = controls && controls->quickSaveLoadHandler;
                if (disabled) controls->quickSaveLoadHandler->inputEventHandlingEnabled = false;
                std::scoped_lock lock(mutex);
                observations["diagnosticNativeQuickSaveLoadDisabled"] = disabled;
            }
        } catch (...) {}
        if (auto* source = RE::TESLoadGameEvent::GetEventSource()) {
            static auto* observer = new LoadObserver;
            source->RegisterSink(observer);
        }
    }
    if (!message || message->type != SFSE::MessagingInterface::kPostPostLoad) return;
    try {
        json config; std::ifstream("Data/SFSE/Plugins/OSFUIBackendTest.json") >> config;
        const auto path = config.at("directory").get<std::string>();
        directory = fs::path(std::u8string(path.begin(), path.end())); session = config.at("session");
        fs::create_directories(directory);
        const bool initialized = api.Init();
        observations["settingsInitialized"] = settings.Init();
        observations["settingsVersion"] = settings.Version();
        bool registered = initialized && api.RegisterSend("osfui-backend-test.report", Report, nullptr)
            && api.RegisterSend("osfui-backend-test.close", Close, nullptr)
            && api.RegisterRequest("osfui-backend-test.echo", Echo, nullptr)
            && api.RegisterRequest("osfui-backend-test.reject", Reject, nullptr)
            && api.RegisterRelativePointer("osfui-backend-test/panel", Pointer, nullptr);
        for (const auto* view : {"osfui-backend-test/panel", "osfui-backend-test/hud", "osfui-backend-test/paused"})
            registered = api.RegisterViewLifecycle(view, Lifecycle, nullptr) && registered;
        if (config.contains("realViews"))
            for (const auto& view : config.at("realViews"))
                registered = api.RegisterViewLifecycle(view.get<std::string>().c_str(), Lifecycle, nullptr) && registered;
        observations["initialized"] = initialized; observations["registered"] = registered;
        observations["apiVersion"] = api.Version();
        const auto acquire = reinterpret_cast<void* (*)(std::uint32_t) noexcept>(GetProcAddress(GetModuleHandleW(L"OSFUI.dll"), "OSFUI_RequestBridge"));
        legacy = acquire ? static_cast<OSFUI::Compat::V1::IOSFUIBridge*>(acquire(0x10008)) : nullptr;
        observations["legacyVersion"] = legacy ? legacy->GetInterfaceVersion() : 0;
        if (legacy) {
            std::uint32_t major{}, minor{}, patch{};
            legacy->GetPluginVersion(major, minor, patch);
            observations["releaseVersion"] = std::to_string(major) + "." + std::to_string(minor) + "." + std::to_string(patch);
            legacy->RegisterCommand("backend.legacy-report", Report, nullptr);
            legacy->RegisterRequest("backend.legacy-echo", LegacyEcho, nullptr);
        }
        REX::INFO("Backend test consumer initialized={}, registered={}, output={}", initialized, registered, directory.string());
        std::thread(Run).detach();
    } catch (const std::exception& error) { REX::ERROR("Backend test consumer: {}", error.what()); }
}
}
SFSE_PLUGIN_PRELOAD(const SFSE::PreLoadInterface*) {
    // A failure must stop this disposable game before production Settings can
    // access unredirected storage. The runner supplies this config only in its
    // new private mod/profile and refuses pre-existing Starfield processes.
    try { if (PreparePrivateStorage()) return true; } catch (...) {}
    ExitProcess(2);
}
SFSE_PLUGIN_LOAD(const SFSE::LoadInterface* sfse) {
    SFSE::Init(sfse);
    return SFSE::GetMessagingInterface()->RegisterListener(OnMessage);
}
