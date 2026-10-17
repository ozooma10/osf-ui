// Disposable SFSE consumer. Exercises the shipped public API without adding
// test hooks to OSFUI.dll. The process owns this plugin for its entire lifetime.
#include <SFSE/SFSE.h>
#include <OSFUI.h>
#include "../../src/Compat/V1/LegacyABI.h"
#include <nlohmann/json.hpp>
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <Windows.h>
#undef ERROR
#include <filesystem>
#include <fstream>
#include <mutex>
#include <thread>

namespace {
using json = nlohmann::json;
namespace fs = std::filesystem;
OSFUI::API::Client api;
OSFUI::Compat::V1::IOSFUIBridge* legacy{};
std::mutex mutex;
json observations = {{"pages", json::object()}, {"pointer", json::object()}, {"lifecycle", json::object()}};
fs::path directory;
std::string session;

void Write(const fs::path& path, const json& value) {
    const auto temporary = path.wstring() + L".tmp";
    { std::ofstream stream(temporary, std::ios::binary); stream << value.dump(); }
    if (!MoveFileExW(temporary.c_str(), path.c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH))
        throw std::runtime_error("Could not publish consumer evidence");
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
                    if (legacy && action == "legacyMenu") accepted = legacy->RequestMenu("osfui-backend-test/legacy", command.value("open", false));
                    if (legacy && action == "legacyState") accepted = legacy->SetViewState("osfui-backend-test", "legacy", command.at("value").dump().c_str());
                    if (legacy && action == "legacyEvent") accepted = legacy->SendToWeb("osfui-backend-test/legacy", "backend.event", command.at("value").dump().c_str());
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
void OnMessage(SFSE::MessagingInterface::Message* message) {
    if (!message || message->type != SFSE::MessagingInterface::kPostPostLoad) return;
    try {
        json config; std::ifstream("Data/SFSE/Plugins/OSFUIBackendTest.json") >> config;
        const auto path = config.at("directory").get<std::string>();
        directory = fs::path(std::u8string(path.begin(), path.end())); session = config.at("session");
        fs::create_directories(directory);
        const bool initialized = api.Init();
        bool registered = initialized && api.RegisterSend("osfui-backend-test.report", Report, nullptr)
            && api.RegisterSend("osfui-backend-test.close", Close, nullptr)
            && api.RegisterRequest("osfui-backend-test.echo", Echo, nullptr)
            && api.RegisterRequest("osfui-backend-test.reject", Reject, nullptr)
            && api.RegisterRelativePointer("osfui-backend-test/panel", Pointer, nullptr);
        for (const auto* view : {"osfui-backend-test/panel", "osfui-backend-test/hud", "osfui-backend-test/paused"})
            registered = api.RegisterViewLifecycle(view, Lifecycle, nullptr) && registered;
        observations["initialized"] = initialized; observations["registered"] = registered;
        observations["apiVersion"] = api.Version();
        const auto acquire = reinterpret_cast<void* (*)(std::uint32_t) noexcept>(GetProcAddress(GetModuleHandleW(L"OSFUI.dll"), "OSFUI_RequestBridge"));
        legacy = acquire ? static_cast<OSFUI::Compat::V1::IOSFUIBridge*>(acquire(0x10008)) : nullptr;
        observations["legacyVersion"] = legacy ? legacy->GetInterfaceVersion() : 0;
        if (legacy) {
            legacy->RegisterCommand("backend.legacy-report", Report, nullptr);
            legacy->RegisterRequest("backend.legacy-echo", LegacyEcho, nullptr);
        }
        REX::INFO("Backend test consumer initialized={}, registered={}, output={}", initialized, registered, directory.string());
        std::thread(Run).detach();
    } catch (const std::exception& error) { REX::ERROR("Backend test consumer: {}", error.what()); }
}
}
SFSE_PLUGIN_LOAD(const SFSE::LoadInterface* sfse) {
    SFSE::Init(sfse);
    return SFSE::GetMessagingInterface()->RegisterListener(OnMessage);
}
