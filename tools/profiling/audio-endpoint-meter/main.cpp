// Read-only endpoint observations. No browser control, volume writes, or PCM files.
#include <windows.h>
#include "AudioMeter.h"
#include <nlohmann/json.hpp>
#include <atomic>
#include <chrono>
#include <cmath>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <memory>
#include <stdexcept>
#include <string>
#include <thread>

namespace fs = std::filesystem;
using json = nlohmann::json;
namespace {
std::atomic_bool cancelled{};
BOOL WINAPI OnControl(DWORD event)
{
    if (event != CTRL_C_EVENT && event != CTRL_BREAK_EVENT) return FALSE;
    cancelled = true;
    return TRUE;
}
std::string UtcNow()
{
    SYSTEMTIME time{};
    GetSystemTime(&time);
    char buffer[40]{};
    snprintf(buffer, sizeof(buffer), "%04u-%02u-%02uT%02u:%02u:%02u.%03uZ",
        time.wYear, time.wMonth, time.wDay, time.wHour, time.wMinute, time.wSecond, time.wMilliseconds);
    return buffer;
}
void Wait(unsigned milliseconds)
{
    const auto end = std::chrono::steady_clock::now() + std::chrono::milliseconds(milliseconds);
    while (std::chrono::steady_clock::now() < end) {
        if (cancelled) throw std::runtime_error("Endpoint measurement cancelled");
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
}
}

int wmain(int argc, wchar_t** argv)
{
    fs::path output;
    unsigned durationMs = 3000, warmupMs = 500;
    double toneHz = 997;
    bool reportWritable = false;
    std::unique_ptr<AudioMeter> meter;
    json report{{"schema",1},{"probe","audio-endpoint-meter"},{"outcome","failed"},
        {"mode","endpoint-read-only"},{"scope","aggregate-endpoint-measurements-no-pcm-retained"}};
    auto save = [&] {
        std::ofstream file(output / "result.json", std::ios::binary);
        file << report.dump(2) << '\n';
        if (!file) throw std::runtime_error("Could not save endpoint measurement report");
    };
    try {
        for (int index = 1; index < argc; ++index) {
            const std::wstring option = argv[index];
            if (index + 1 >= argc) throw std::runtime_error("Missing option value");
            const std::wstring value = argv[++index];
            if (option == L"--output") output = fs::absolute(value);
            else if (option == L"--duration-ms") durationMs = std::stoul(value);
            else if (option == L"--warmup-ms") warmupMs = std::stoul(value);
            else if (option == L"--tone-hz") toneHz = std::stod(value);
            else throw std::runtime_error("Unknown option");
        }
        if (output.empty() || durationMs < 1000 || durationMs > 30000 || warmupMs > 5000 ||
            !std::isfinite(toneHz) || toneHz < 1 || toneHz > 20000)
            throw std::runtime_error("Required: --output DIR [--duration-ms 1000..30000] [--warmup-ms 0..5000] [--tone-hz 1..20000]");
        if (fs::exists(output / "result.json")) throw std::runtime_error("Refusing to overwrite existing result.json");
        fs::create_directories(output);
        reportWritable = true;
        report["durationMs"] = durationMs;
        report["warmupMs"] = warmupMs;
        report["toneHz"] = toneHz;
        report["startedAtUtc"] = UtcNow();
        report["outcome"] = "running";
        save();
        SetConsoleCtrlHandler(OnControl, TRUE);
        meter = std::make_unique<AudioMeter>(toneHz);
        if (!meter->Start()) throw std::runtime_error("Endpoint meter initialization failed");
        Wait(warmupMs);
        if (!meter->Reset()) throw std::runtime_error("Endpoint meter reset failed");
        report["measurementStartedAtUtc"] = UtcNow();
        Wait(durationMs);
        report["measurementEndedAtUtc"] = UtcNow();
        report["endpoint"] = meter->Snapshot();
        const auto& endpoint = report["endpoint"];
        if (!endpoint.value("ready",false) || !endpoint.value("running",false) || !endpoint["error"].is_null())
            throw std::runtime_error("Endpoint meter stopped or reported an error");
        meter->Stop();
        report["outcome"] = "measured";
        report["exitCode"] = 0;
        report["completedAtUtc"] = UtcNow();
        report["limits"] = json::array({
            "Measurement includes all audio rendered to the default console endpoint; it is not process-isolated.",
            "Zero packets may mean an idle endpoint, not meter failure.",
            "Exit 0 confirms measurement only; source attribution and scenario acceptance require surrounding evidence."});
        save();
        std::cout << report.dump() << '\n';
        return 0;
    } catch (const std::exception& error) {
        if (meter) { report["endpoint"] = meter->Snapshot(); meter->Stop(); }
        report["outcome"] = "failed";
        report["exitCode"] = 1;
        report["error"] = error.what();
        report["completedAtUtc"] = UtcNow();
        if (reportWritable) {
            try { save(); } catch (...) {}
        }
        std::cerr << report.dump() << '\n';
        return 1;
    }
}
