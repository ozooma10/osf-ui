// Standalone feasibility probe. No hook, injection, or shipping host changes.
// Captures one owned WebView2 host tree and temporarily mutes only its sessions.
#include <windows.h>
#include <audioclient.h>
#include <audioclientactivationparams.h>
#include <audiopolicy.h>
#include <mmdeviceapi.h>
#include <tlhelp32.h>
#include <wrl.h>
#include <nlohmann/json.hpp>

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <map>
#include <numbers>
#include <set>
#include <stdexcept>
#include <string>
#include <vector>

using Microsoft::WRL::ComPtr;
using json = nlohmann::json;
namespace fs = std::filesystem;

namespace {
constexpr unsigned kRate = 48000;
constexpr unsigned kChannels = 2;
std::atomic_bool cancelled{false};

void Check(HRESULT hr, const char* operation)
{
    if (FAILED(hr)) {
        char value[24]{};
        snprintf(value, sizeof(value), "0x%08lX", static_cast<unsigned long>(hr));
        throw std::runtime_error(std::string(operation) + ": " + value);
    }
}

struct Handle {
    HANDLE value{};
    explicit Handle(HANDLE h = nullptr) : value(h) {}
    ~Handle() { if (value && value != INVALID_HANDLE_VALUE) CloseHandle(value); }
    Handle(const Handle&) = delete;
    Handle& operator=(const Handle&) = delete;
};

std::uint64_t Started(HANDLE process)
{
    FILETIME creation{}, exit{}, kernel{}, user{};
    if (!GetProcessTimes(process, &creation, &exit, &kernel, &user))
        throw std::runtime_error("GetProcessTimes failed");
    return (std::uint64_t(creation.dwHighDateTime) << 32) | creation.dwLowDateTime;
}

std::wstring Image(HANDLE process)
{
    std::wstring path(32768, L'\0');
    DWORD count = static_cast<DWORD>(path.size());
    if (!QueryFullProcessImageNameW(process, 0, path.data(), &count))
        throw std::runtime_error("QueryFullProcessImageName failed");
    path.resize(count);
    return path;
}

class Owner {
public:
    Handle handle;
    DWORD pid;
    std::uint64_t start;
    Owner(DWORD id, std::uint64_t expected, bool cef = false) :
        handle(OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION | SYNCHRONIZE, FALSE, id)),
        pid(id), start(expected)
    {
        if (!handle.value || Started(handle.value) != expected)
            throw std::runtime_error("Host PID/creation time mismatch");
        if (_wcsicmp(fs::path(Image(handle.value)).filename().c_str(),
                cef ? L"osfui_cef_host.exe" : L"osfui_webview2_host.exe") != 0)
            throw std::runtime_error("Target must match the selected OSF UI browser host");
        Alive();
    }
    void Alive() const
    {
        if (WaitForSingleObject(handle.value, 0) != WAIT_TIMEOUT)
            throw std::runtime_error("Owned browser host exited");
        if (cancelled) throw std::runtime_error("Cancelled");
    }
    std::set<DWORD> Tree() const
    {
        Alive();
        Handle snapshot(CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0));
        if (snapshot.value == INVALID_HANDLE_VALUE)
            throw std::runtime_error("Process snapshot failed");
        struct Child { DWORD parent; std::uint64_t start; };
        std::map<DWORD, Child> candidates;
        PROCESSENTRY32W entry{sizeof(entry)};
        if (!Process32FirstW(snapshot.value, &entry))
            throw std::runtime_error("Process enumeration failed");
        do {
            if (_wcsicmp(entry.szExeFile, L"msedgewebview2.exe") != 0) continue;
            Handle process(OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, entry.th32ProcessID));
            if (!process.value) continue;
            try {
                candidates.emplace(entry.th32ProcessID, Child{entry.th32ParentProcessID, Started(process.value)});
            } catch (...) { /* An exiting child cannot own a live capture session. */ }
        } while (Process32NextW(snapshot.value, &entry));
        std::map<DWORD, std::uint64_t> tree{{pid, start}};
        bool added;
        do {
            added = false;
            for (auto [id, child] : candidates) {
                const auto parent = tree.find(child.parent);
                if (parent != tree.end() && child.start >= parent->second && !tree.contains(id)) {
                    tree.emplace(id, child.start);
                    added = true;
                }
            }
        } while (added);
        std::set<DWORD> ids;
        for (auto [id, unused] : tree) ids.insert(id);
        return ids;
    }
};

class Activation final : public Microsoft::WRL::RuntimeClass<
    Microsoft::WRL::RuntimeClassFlags<Microsoft::WRL::ClassicCom>,
    IActivateAudioInterfaceCompletionHandler, Microsoft::WRL::FtmBase> {
public:
    Handle done{CreateEventW(nullptr, TRUE, FALSE, nullptr)};
    HRESULT result = E_PENDING;
    ComPtr<IAudioClient> client;
    HRESULT STDMETHODCALLTYPE ActivateCompleted(IActivateAudioInterfaceAsyncOperation* op) override
    {
        ComPtr<IUnknown> object;
        HRESULT activation = E_FAIL;
        result = op->GetActivateResult(&activation, &object);
        if (SUCCEEDED(result)) result = activation;
        if (SUCCEEDED(result)) result = object.As(&client);
        SetEvent(done.value);
        return S_OK;
    }
};

struct Stats {
    std::uint64_t frames{}, silentFrames{}, packets{}, discontinuities{};
    std::uint64_t firstQpc{}, lastQpc{};
    double squares{}, peak{}, toneSquares{}, re{}, im{};
    double channelSquares[2]{};
    unsigned blockFrames{}, toneBlocks{};
    void Add(const std::int16_t* data, UINT32 count, DWORD flags, UINT64 qpc, double hz)
    {
        if (!packets++) firstQpc = qpc;
        lastQpc = qpc;
        if (flags & AUDCLNT_BUFFERFLAGS_DATA_DISCONTINUITY) ++discontinuities;
        const bool silent = (flags & AUDCLNT_BUFFERFLAGS_SILENT) != 0;
        if (silent) silentFrames += count;
        for (UINT32 f = 0; f < count; ++f) {
            double mono = 0;
            for (unsigned c = 0; c < kChannels; ++c) {
                const double sample = silent ? 0.0 : data[f * kChannels + c] / 32768.0;
                squares += sample * sample;
                channelSquares[c] += sample * sample;
                peak = std::max(peak, std::abs(sample));
                mono += sample / kChannels;
            }
            if (hz > 0) {
                const double angle = 2 * std::numbers::pi * hz * blockFrames / kRate;
                re += mono * std::cos(angle);
                im += mono * std::sin(angle);
                // Non-overlapping 100 ms windows; average power, independent of phase.
                if (++blockFrames == kRate / 10) {
                    toneSquares += 2 * (re * re + im * im) / (blockFrames * blockFrames);
                    ++toneBlocks;
                    blockFrames = 0; re = 0; im = 0;
                }
            }
        }
        frames += count;
    }
    json Report() const
    {
        return {{"frames", frames}, {"packets", packets}, {"silentFrames", silentFrames},
            {"discontinuities", discontinuities}, {"firstQpc100ns", firstQpc},
            {"lastQpc100ns", lastQpc}, {"seconds", double(frames) / kRate},
            {"rms", frames ? std::sqrt(squares / (frames * kChannels)) : 0},
            {"peak", peak}, {"toneRms", toneBlocks ? std::sqrt(toneSquares / toneBlocks) : 0},
            {"leftRms",frames ? std::sqrt(channelSquares[0] / frames) : 0},
            {"rightRms",frames ? std::sqrt(channelSquares[1] / frames) : 0}};
    }
};

class Capture {
public:
    ComPtr<IAudioClient> client;
    ComPtr<IAudioCaptureClient> reader;
    Handle ready{CreateEventW(nullptr, FALSE, FALSE, nullptr)};
    Stats stats;
    std::vector<std::int16_t> samples;
    bool saveSamples = false;
    ~Capture() { if (client) client->Stop(); }
    void Process(DWORD pid)
    {
        AUDIOCLIENT_ACTIVATION_PARAMS params{};
        params.ActivationType = AUDIOCLIENT_ACTIVATION_TYPE_PROCESS_LOOPBACK;
        params.ProcessLoopbackParams = {pid, PROCESS_LOOPBACK_MODE_INCLUDE_TARGET_PROCESS_TREE};
        PROPVARIANT variant{};
        variant.vt = VT_BLOB;
        variant.blob = {sizeof(params), reinterpret_cast<BYTE*>(&params)};
        auto completion = Microsoft::WRL::Make<Activation>();
        if (!completion->done.value) throw std::runtime_error("Activation event failed");
        ComPtr<IActivateAudioInterfaceAsyncOperation> operation;
        Check(ActivateAudioInterfaceAsync(VIRTUAL_AUDIO_DEVICE_PROCESS_LOOPBACK,
            __uuidof(IAudioClient), &variant, completion.Get(), &operation), "Activate process loopback");
        if (WaitForSingleObject(completion->done.value, 5000) != WAIT_OBJECT_0)
            throw std::runtime_error("Process loopback activation timed out");
        Check(completion->result, "Process loopback activation result");
        client = completion->client;
        Initialize();
    }
    void Endpoint(IMMDevice* device)
    {
        Check(device->Activate(__uuidof(IAudioClient), CLSCTX_ALL, nullptr,
            reinterpret_cast<void**>(client.GetAddressOf())), "Activate endpoint loopback");
        Initialize();
    }
    void Initialize()
    {
        if (!ready.value) throw std::runtime_error("Capture event failed");
        WAVEFORMATEX format{WAVE_FORMAT_PCM, kChannels, kRate, kRate * kChannels * 2,
            kChannels * 2, 16, 0};
        Check(client->Initialize(AUDCLNT_SHAREMODE_SHARED,
            AUDCLNT_STREAMFLAGS_LOOPBACK | AUDCLNT_STREAMFLAGS_EVENTCALLBACK |
            AUDCLNT_STREAMFLAGS_AUTOCONVERTPCM, 0, 0, &format, nullptr), "Initialize loopback");
        Check(client->GetService(IID_PPV_ARGS(&reader)), "Get capture service");
        Check(client->SetEventHandle(ready.value), "Set capture event");
        Check(client->Start(), "Start capture");
    }
    void Drain(double hz, bool discard = false)
    {
        UINT32 available = 0;
        for (;;) {
            Check(reader->GetNextPacketSize(&available), "Next audio packet");
            if (!available) break;
            BYTE* bytes = nullptr;
            UINT32 frames = 0;
            DWORD flags = 0;
            UINT64 position = 0, qpc = 0;
            Check(reader->GetBuffer(&bytes, &frames, &flags, &position, &qpc), "Get audio packet");
            try {
                if (!discard) {
                    auto* data = reinterpret_cast<std::int16_t*>(bytes);
                    stats.Add(data, frames, flags, qpc, hz);
                    if (saveSamples) {
                        const auto old = samples.size();
                        samples.resize(old + frames * kChannels, 0);
                        if (!(flags & AUDCLNT_BUFFERFLAGS_SILENT))
                            std::copy_n(data, frames * kChannels, samples.data() + old);
                    }
                }
            } catch (...) { reader->ReleaseBuffer(frames); throw; }
            Check(reader->ReleaseBuffer(frames), "Release audio packet");
        }
    }
    void Reset()
    {
        Drain(0, true);
        stats = {};
        samples.clear();
    }
};

struct Session {
    ComPtr<ISimpleAudioVolume> volume;
    std::wstring id;
    DWORD pid{};
    BOOL originalMute{};
    float originalVolume{};
    bool changed{};
};

class Suppression {
public:
    std::vector<Session> sessions;
    GUID context{};
    json& report;
    Suppression(IMMDevice* endpoint, const Owner& owner, json& outputReport) : report(outputReport)
    {
        Check(CoCreateGuid(&context), "Create audio context");
        ComPtr<IAudioSessionManager2> manager;
        Check(endpoint->Activate(__uuidof(IAudioSessionManager2), CLSCTX_ALL, nullptr,
            reinterpret_cast<void**>(manager.GetAddressOf())), "Activate session manager");
        ComPtr<IAudioSessionEnumerator> list;
        Check(manager->GetSessionEnumerator(&list), "Enumerate sessions");
        int count = 0;
        Check(list->GetCount(&count), "Session count");
        const auto tree = owner.Tree();
        for (int index = 0; index < count; ++index) {
            ComPtr<IAudioSessionControl> base;
            ComPtr<IAudioSessionControl2> control;
            Check(list->GetSession(index, &base), "Get session");
            Check(base.As(&control), "Get session control");
            DWORD pid = 0;
            const HRESULT result = control->GetProcessId(&pid);
            if (result != S_OK || !tree.contains(pid)) continue;
            AudioSessionState state;
            Check(control->GetState(&state), "Read session state");
            if (state != AudioSessionStateActive) continue;
            Session session;
            session.pid = pid;
            Check(control.As(&session.volume), "Get session volume");
            Check(session.volume->GetMute(&session.originalMute), "Read original mute");
            Check(session.volume->GetMasterVolume(&session.originalVolume), "Read original volume");
            LPWSTR id = nullptr;
            Check(control->GetSessionInstanceIdentifier(&id), "Session identity");
            session.id = id;
            CoTaskMemFree(id);
            sessions.push_back(std::move(session));
        }
        if (sessions.empty()) throw std::runtime_error("No active host-owned audio session on default output");
        for (const auto& session : sessions) {
            if (session.originalMute || session.originalVolume < 0.01f)
                throw std::runtime_error("Browser already muted/near silent; baseline would be inconclusive");
        }
    }
    ~Suppression()
    {
        const bool restored = Restore();
        try {
            report["restored"] = restored;
            report["finalSessions"] = Report();
            if (!restored) {
                report["outcome"] = "failed";
                report["restorationError"] = "Could not restore original browser session mute state";
            }
        } catch (...) {
            // A retired session may no longer answer queries; do not conceal restore failure.
            if (!restored) std::cerr << "Browser session restoration failed\n";
        }
    }
    void Mute()
    {
        for (auto& session : sessions) {
            // Mark first so a failed readback still triggers restoration.
            session.changed = true;
            Check(session.volume->SetMute(TRUE, &context), "Mute owned browser session");
            BOOL muted = FALSE;
            Check(session.volume->GetMute(&muted), "Read muted session");
            if (!muted) throw std::runtime_error("Session mute did not take effect");
        }
    }
    bool Restore() noexcept
    {
        bool ok = true;
        for (auto& session : sessions) {
            if (!session.changed) continue;
            BOOL muted = !session.originalMute;
            if (FAILED(session.volume->SetMute(session.originalMute, &context)) ||
                FAILED(session.volume->GetMute(&muted)) || muted != session.originalMute) {
                ok = false;
            } else {
                session.changed = false;
            }
        }
        return ok;
    }
    json Report() const
    {
        json result = json::array();
        for (const auto& session : sessions) {
            BOOL mute = FALSE;
            float volume = 0;
            Check(session.volume->GetMute(&mute), "Read session mute");
            Check(session.volume->GetMasterVolume(&volume), "Read session volume");
            result.push_back({{"pid", session.pid}, {"originalMute", bool(session.originalMute)},
                {"muted", bool(mute)}, {"originalVolume", session.originalVolume}, {"volume", volume}});
        }
        return result;
    }
};

void SaveWav(const fs::path& path, const std::vector<std::int16_t>& samples)
{
    std::ofstream out(path, std::ios::binary);
    auto u32 = [&](std::uint32_t n) { out.write(reinterpret_cast<const char*>(&n), 4); };
    auto u16 = [&](std::uint16_t n) { out.write(reinterpret_cast<const char*>(&n), 2); };
    const auto bytes = static_cast<std::uint32_t>(samples.size() * 2);
    out.write("RIFF", 4); u32(36 + bytes); out.write("WAVEfmt ", 8); u32(16);
    u16(1); u16(kChannels); u32(kRate); u32(kRate * kChannels * 2);
    u16(kChannels * 2); u16(16); out.write("data", 4); u32(bytes);
    out.write(reinterpret_cast<const char*>(samples.data()), bytes);
    if (!out) throw std::runtime_error("Could not save process WAV");
}

BOOL WINAPI OnControl(DWORD event)
{
    if (event == CTRL_C_EVENT || event == CTRL_BREAK_EVENT) {
        cancelled = true;
        return TRUE;
    }
    return FALSE;
}
} // namespace

int wmain(int argc, wchar_t** argv)
{
    json report{{"schemaVersion", 1}, {"outcome", "failed"},
        {"sampleRate", kRate}, {"channels", kChannels}};
    fs::path output;
    bool com = false;
    bool reportWritable = false;
    int code = 1;
    try {
        DWORD pid = 0;
        std::uint64_t start = 0;
        unsigned phaseMs = 3000;
        double hz = 997;
        bool wav = false;
        bool captureRoot = false;
        bool cefMeter = false;
        for (int i = 1; i < argc; ++i) {
            const std::wstring arg = argv[i];
            if (arg == L"--wav") { wav = true; continue; }
            if (arg == L"--capture-root") { captureRoot = true; continue; }
            if (arg == L"--cef-meter") { cefMeter = true; continue; }
            if (i + 1 == argc) throw std::runtime_error("Missing option value");
            const std::wstring value = argv[++i];
            if (arg == L"--pid") pid = std::stoul(value);
            else if (arg == L"--start-time") start = std::stoull(value);
            else if (arg == L"--output") output = value;
            else if (arg == L"--phase-ms") phaseMs = std::stoul(value);
            else if (arg == L"--tone-hz") hz = std::stod(value);
            else throw std::runtime_error("Unknown option");
        }
        if (!pid || !start || output.empty() || phaseMs < 1000 || phaseMs > 10000 ||
            !std::isfinite(hz) || hz < 0 || hz > 20000)
            throw std::runtime_error("Required: --pid ID --start-time FILETIME --output DIR [--tone-hz 997|0] [--wav]");
        fs::create_directories(output);
        if (fs::exists(output / "result.json")) throw std::runtime_error("Choose a fresh output directory");
        reportWritable = true;
        Check(CoInitializeEx(nullptr, COINIT_MULTITHREADED), "Initialize COM");
        com = true;
        SetConsoleCtrlHandler(OnControl, TRUE);
        Owner owner(pid, start, cefMeter);
        report["hostPid"] = pid;
        report["hostStartFileTime"] = std::to_string(start);
        report["toneHz"] = hz;
        report["phaseMs"] = phaseMs;
        ComPtr<IMMDeviceEnumerator> enumerator;
        Check(CoCreateInstance(__uuidof(MMDeviceEnumerator), nullptr, CLSCTX_ALL,
            IID_PPV_ARGS(&enumerator)), "Device enumerator");
        ComPtr<IMMDevice> device;
        Check(enumerator->GetDefaultAudioEndpoint(eRender, eConsole, &device), "Default render endpoint");
        LPWSTR deviceId = nullptr;
        Check(device->GetId(&deviceId), "Endpoint ID");
        report["endpointId"] = fs::path(deviceId).string();
        CoTaskMemFree(deviceId);
        if (cefMeter) {
            // Read-only meter: never acquires session volume or calls SetMute.
            Capture process, endpoint;
            process.saveSamples = wav;
            process.Process(pid); endpoint.Endpoint(device.Get());
            const auto until = GetTickCount64() + phaseMs;
            while (GetTickCount64() < until) {
                owner.Alive();
                process.Drain(hz); endpoint.Drain(hz); Sleep(5);
            }
            report["process"] = process.stats.Report();
            report["endpoint"] = endpoint.stats.Report();
            report["mode"] = "cef-meter-read-only";
            report["outcome"] = "passed";
            if (wav) SaveWav(output / "output.wav", process.samples);
            code = 0;
        } else {
        Suppression suppression(device.Get(), owner, report);
        std::set<DWORD> audioPids;
        for (const auto& session : suppression.sessions) audioPids.insert(session.pid);
        if (!captureRoot && audioPids.size() != 1)
            throw std::runtime_error("Expected one owned audio process; multiple sources need separate captures");
        const DWORD capturePid = captureRoot ? pid : *audioPids.begin();
        Handle captureOwner(OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION | SYNCHRONIZE, FALSE, capturePid));
        if (!captureOwner.value || !owner.Tree().contains(capturePid))
            throw std::runtime_error("Audio process no longer belongs to owned host");
        report["capturePid"] = capturePid;
        report["captureStartFileTime"] = std::to_string(Started(captureOwner.value));
        Capture process, endpoint;
        process.saveSamples = wav;
        process.Process(capturePid);
        endpoint.Endpoint(device.Get());
        HANDLE events[]{process.ready.value, endpoint.ready.value};
        auto pump = [&](unsigned ms, bool discard) {
            const auto deadline = GetTickCount64() + ms;
            while (GetTickCount64() < deadline) {
                owner.Alive();
                if (WaitForSingleObject(captureOwner.value, 0) != WAIT_TIMEOUT)
                    throw std::runtime_error("Owned audio process exited");
                const auto wait = WaitForMultipleObjects(2, events, FALSE, 25);
                if (wait == WAIT_FAILED) throw std::runtime_error("Capture wait failed");
                process.Drain(hz, discard);
                endpoint.Drain(hz, discard);
            }
        };
        for (const char* phase : {"baseline", "suppressed", "restored"}) {
            owner.Alive();
            if (std::string(phase) == "suppressed") suppression.Mute();
            if (std::string(phase) == "restored" && !suppression.Restore())
                throw std::runtime_error("Failed to restore browser mute state");
            pump(350, true); // Exclude transition packets from all three measurements.
            process.Reset(); endpoint.Reset();
            std::cout << "Measuring " << phase << std::endl;
            pump(phaseMs, false);
            report["phases"][phase] = {{"process", process.stats.Report()},
                {"endpoint", endpoint.stats.Report()}, {"sessions", suppression.Report()}};
            if (wav) SaveWav(output / (std::string(phase) + ".wav"), process.samples);
        }
        report["restored"] = suppression.Restore();
        const auto& phases = report["phases"];
        const auto rms = [&](const char* phase, const char* stream, const char* field) {
            return phases[phase][stream][field].get<double>();
        };
        const char* metric = hz > 0 ? "toneRms" : "rms";
        const double base = rms("baseline", "process", metric);
        const double muted = rms("suppressed", "process", metric);
        const double recovered = rms("restored", "process", metric);
        const double endpointBase = rms("baseline", "endpoint", metric);
        const double endpointMuted = rms("suppressed", "endpoint", metric);
        const double endpointRecovered = rms("restored", "endpoint", metric);
        report["capturedDuringSuppression"] = base > 0.0001 && muted > 0.0001 && recovered > 0.0001;
        if (hz > 0) {
            report["processTonePreserved"] = base > 0.0001 && muted > base * 0.8 && muted < base * 1.2 &&
                recovered > base * 0.8 && recovered < base * 1.2;
            report["outputToneSuppressed"] = endpointBase > 0.0001 && endpointMuted < endpointBase * 0.1 &&
                endpointRecovered > endpointBase * 0.8 && endpointRecovered < endpointBase * 1.2;
        }
        const bool passed = report["restored"].get<bool>() && report["capturedDuringSuppression"].get<bool>() &&
            (hz == 0 || (report["processTonePreserved"].get<bool>() && report["outputToneSuppressed"].get<bool>()));
        report["outcome"] = passed ? "passed" : "failed";
        code = passed ? 0 : 2;
        }
    } catch (const std::exception& e) {
        report["error"] = e.what();
    }
    if (com) CoUninitialize();
    if (reportWritable) {
        std::ofstream result(output / "result.json");
        result << report.dump(2) << '\n';
        if (!result) { std::cerr << "Unable to save report\n"; return 1; }
    }
    std::cout << report.dump(2) << std::endl;
    return report["outcome"] == "passed" ? code : (code == 0 ? 1 : code);
}
