#include "AudioMeter.h"

#include <windows.h>
#include <audioclient.h>
#include <mmdeviceapi.h>
#include <mmreg.h>
#include <ks.h>
#include <ksmedia.h>
#include <wrl/client.h>
#include <nlohmann/json.hpp>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <condition_variable>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <mutex>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

using Microsoft::WRL::ComPtr;
using json = nlohmann::json;

namespace {
using Clock = std::chrono::steady_clock;
constexpr double kPi = 3.14159265358979323846;

void Check(HRESULT result, const char* operation)
{
    if (FAILED(result)) {
        char code[24]{};
        std::snprintf(code, sizeof(code), "0x%08lX", static_cast<unsigned long>(result));
        throw std::runtime_error(std::string(operation) + ": " + code);
    }
}

struct Handle {
    HANDLE value{};
    ~Handle() { if (value) CloseHandle(value); }
    Handle() = default;
    Handle(const Handle&) = delete;
    Handle& operator=(const Handle&) = delete;
    void Create(bool manualReset)
    {
        if (value) CloseHandle(value);
        value = CreateEventW(nullptr, manualReset ? TRUE : FALSE, FALSE, nullptr);
        if (!value)
            throw std::runtime_error("CreateEventW failed: " + std::to_string(GetLastError()));
    }
};

struct ComScope {
    ComScope() { Check(CoInitializeEx(nullptr, COINIT_MULTITHREADED), "CoInitializeEx"); }
    ~ComScope() { CoUninitialize(); }
};

template<class T>
struct ComMemory {
    T* value{};
    ~ComMemory() { CoTaskMemFree(value); }
};

struct StopClient {
    IAudioClient* client{};
    ~StopClient() { if (client) client->Stop(); }
};

std::string Utf8(const wchar_t* value)
{
    if (!value || !*value) return {};
    const int length = WideCharToMultiByte(CP_UTF8, 0, value, -1, nullptr, 0, nullptr, nullptr);
    if (!length) throw std::runtime_error("Converting endpoint ID failed");
    std::string result(static_cast<std::size_t>(length), '\0');
    if (!WideCharToMultiByte(CP_UTF8, 0, value, -1, result.data(), length, nullptr, nullptr))
        throw std::runtime_error("Converting endpoint ID failed");
    result.pop_back();
    return result;
}

struct Format {
    unsigned rate{}, channels{}, bits{}, blockAlign{}, channelMask{};
    bool floating{};

    explicit Format(const WAVEFORMATEX& wave) :
        rate(wave.nSamplesPerSec), channels(wave.nChannels),
        bits(wave.wBitsPerSample), blockAlign(wave.nBlockAlign)
    {
        WORD tag = wave.wFormatTag;
        if (tag == WAVE_FORMAT_EXTENSIBLE) {
            if (wave.cbSize < sizeof(WAVEFORMATEXTENSIBLE) - sizeof(WAVEFORMATEX))
                throw std::runtime_error("Truncated extensible endpoint mix format");
            const auto& extended = reinterpret_cast<const WAVEFORMATEXTENSIBLE&>(wave);
            channelMask = extended.dwChannelMask;
            if (IsEqualGUID(extended.SubFormat, KSDATAFORMAT_SUBTYPE_IEEE_FLOAT))
                tag = WAVE_FORMAT_IEEE_FLOAT;
            else if (IsEqualGUID(extended.SubFormat, KSDATAFORMAT_SUBTYPE_PCM))
                tag = WAVE_FORMAT_PCM;
            else
                throw std::runtime_error("Unsupported endpoint mix-format subformat");
        } else if (channels == 2) {
            channelMask = SPEAKER_FRONT_LEFT | SPEAKER_FRONT_RIGHT;
        } else if (channels == 1) {
            channelMask = SPEAKER_FRONT_CENTER;
        }
        floating = tag == WAVE_FORMAT_IEEE_FLOAT;
        const bool validBits = floating ? (bits == 32 || bits == 64) :
            (bits == 8 || bits == 16 || bits == 24 || bits == 32);
        if ((tag != WAVE_FORMAT_PCM && !floating) || !validBits ||
            rate < 8000 || rate > 384000 || !channels || channels > 32 ||
            blockAlign < channels * (bits / 8))
            throw std::runtime_error("Unsupported endpoint mix format");
    }

    double Sample(const BYTE* bytes) const
    {
        if (floating) {
            if (bits == 32) {
                float value{};
                std::memcpy(&value, bytes, sizeof(value));
                return value;
            }
            double value{};
            std::memcpy(&value, bytes, sizeof(value));
            return value;
        }
        switch (bits) {
        case 8:
            return (static_cast<int>(bytes[0]) - 128) / 128.0;
        case 16: {
            std::int16_t value{};
            std::memcpy(&value, bytes, sizeof(value));
            return value / 32768.0;
        }
        case 24: {
            std::int32_t value = static_cast<std::int32_t>(bytes[0]) |
                (static_cast<std::int32_t>(bytes[1]) << 8) |
                (static_cast<std::int32_t>(bytes[2]) << 16);
            if (value & 0x00800000) value -= 0x01000000;
            return value / 8388608.0;
        }
        default: {
            std::int32_t value{};
            std::memcpy(&value, bytes, sizeof(value));
            return value / 2147483648.0;
        }
        }
    }

    json Report() const
    {
        return {{"sampleRate", rate}, {"channels", channels}, {"bitsPerSample", bits},
            {"blockAlign", blockAlign}, {"channelMask", channelMask},
            {"encoding", floating ? "ieee-float" : "pcm"}};
    }
};

struct Stats {
    std::uint64_t frames{}, silentFrames{}, packets{}, discontinuities{}, timestampErrors{};
    std::uint64_t invalidSamples{}, firstQpc100ns{}, lastQpc100ns{}, toneBlocks{};
    unsigned toneFrames{};
    std::vector<double> squares, peak, real, imaginary, toneSquares;
    Clock::time_point started{Clock::now()};

    explicit Stats(unsigned channels = 0) :
        squares(channels), peak(channels), real(channels),
        imaginary(channels), toneSquares(channels) {}

    void ResetTone()
    {
        toneFrames = 0;
        std::fill(real.begin(), real.end(), 0.0);
        std::fill(imaginary.begin(), imaginary.end(), 0.0);
    }

    void Add(const BYTE* bytes, UINT32 count, DWORD flags, UINT64 qpc,
        const Format& format, double toneHz)
    {
        if (!packets++) firstQpc100ns = qpc;
        lastQpc100ns = qpc;
        if (flags & AUDCLNT_BUFFERFLAGS_DATA_DISCONTINUITY) {
            ++discontinuities;
            ResetTone();
        }
        if (flags & AUDCLNT_BUFFERFLAGS_TIMESTAMP_ERROR) ++timestampErrors;
        const bool silent = (flags & AUDCLNT_BUFFERFLAGS_SILENT) != 0;
        if (silent) silentFrames += count;
        if (!silent && !bytes) throw std::runtime_error("Non-silent packet has no audio data");
        const unsigned toneWindow = format.rate / 10;
        for (UINT32 frame = 0; frame < count; ++frame) {
            const double angle = 2.0 * kPi * toneHz * toneFrames / format.rate;
            const double cosine = std::cos(angle), sine = std::sin(angle);
            for (unsigned channel = 0; channel < format.channels; ++channel) {
                double value = silent ? 0.0 : format.Sample(bytes +
                    static_cast<std::size_t>(frame) * format.blockAlign + channel * (format.bits / 8));
                if (!std::isfinite(value)) {
                    ++invalidSamples;
                    value = 0.0;
                }
                squares[channel] += value * value;
                peak[channel] = (std::max)(peak[channel], std::abs(value));
                real[channel] += value * cosine;
                imaginary[channel] += value * sine;
            }
            if (++toneFrames == toneWindow) {
                const double divisor = static_cast<double>(toneWindow) * toneWindow;
                for (unsigned channel = 0; channel < format.channels; ++channel)
                    toneSquares[channel] += 2.0 *
                        (real[channel] * real[channel] + imaginary[channel] * imaginary[channel]) / divisor;
                ++toneBlocks;
                ResetTone();
            }
        }
        frames += count;
    }

    json Report(unsigned rate, double toneHz, Clock::time_point now) const
    {
        std::vector<double> rms(squares.size()), toneRms(squares.size());
        double sumSquares = 0.0, maxPeak = 0.0, sumTone = 0.0;
        const double frameCount = static_cast<double>(frames);
        const double blockCount = static_cast<double>(toneBlocks);
        for (std::size_t channel = 0; channel < squares.size(); ++channel) {
            rms[channel] = frames ? std::sqrt(squares[channel] / frameCount) : 0.0;
            toneRms[channel] = toneBlocks ? std::sqrt(toneSquares[channel] / blockCount) : 0.0;
            sumSquares += squares[channel];
            sumTone += toneSquares[channel];
            maxPeak = (std::max)(maxPeak, peak[channel]);
        }
        const double channels = static_cast<double>(squares.size());
        return {{"frames", frames}, {"silentFrames", silentFrames}, {"packets", packets},
            {"discontinuities", discontinuities}, {"timestampErrors", timestampErrors},
            {"invalidSamples", invalidSamples},
            {"firstQpc100ns", firstQpc100ns}, {"lastQpc100ns", lastQpc100ns},
            {"seconds", rate ? frameCount / rate : 0.0},
            {"wallSeconds", std::chrono::duration<double>(now - started).count()},
            {"rms", frames && channels ? std::sqrt(sumSquares / (frameCount * channels)) : 0.0},
            {"peak", maxPeak}, {"channelRms", rms}, {"channelPeak", peak},
            {"channelEnergy", squares},
            {"toneHz", toneHz}, {"toneWindowSeconds", 0.1}, {"toneBlocks", toneBlocks},
            {"toneRms", toneBlocks && channels ? std::sqrt(sumTone / (blockCount * channels)) : 0.0},
            {"channelToneRms", toneRms}};
    }
};
}

struct AudioMeter::Impl {
    explicit Impl(double frequency) : toneHz(frequency) {}

    const double toneHz;
    std::mutex lifecycleMutex;
    mutable std::mutex mutex;
    std::condition_variable changed;
    std::thread worker;
    Handle stopEvent, resetEvent;
    bool startupComplete{}, ready{}, running{};
    std::uint64_t requestedGeneration{}, generation{};
    unsigned sampleRate{};
    std::string endpointId, error;
    json formatReport;
    Stats stats;
    Clock::time_point ended{Clock::now()};

    void Fail(const std::string& message)
    {
        {
            std::lock_guard lock(mutex);
            if (error.empty()) error = message;
            running = false;
            ended = Clock::now();
            startupComplete = true;
        }
        changed.notify_all();
    }

    void Run()
    {
        try {
            ComScope com;
            ComPtr<IMMDeviceEnumerator> enumerator;
            Check(CoCreateInstance(__uuidof(MMDeviceEnumerator), nullptr, CLSCTX_ALL,
                IID_PPV_ARGS(&enumerator)), "Create endpoint enumerator");
            ComPtr<IMMDevice> endpoint;
            Check(enumerator->GetDefaultAudioEndpoint(eRender, eConsole, &endpoint),
                "Get default render endpoint");
            ComMemory<wchar_t> id;
            Check(endpoint->GetId(&id.value), "Get endpoint ID");
            ComPtr<IAudioClient> client;
            Check(endpoint->Activate(__uuidof(IAudioClient), CLSCTX_ALL, nullptr,
                reinterpret_cast<void**>(client.GetAddressOf())), "Activate endpoint loopback");
            ComMemory<WAVEFORMATEX> mixFormat;
            Check(client->GetMixFormat(&mixFormat.value), "Get endpoint mix format");
            Format format(*mixFormat.value);
            if (!std::isfinite(toneHz) || toneHz <= 0.0 || toneHz >= format.rate / 2.0)
                throw std::runtime_error("Meter tone frequency is outside the valid range");
            Handle packetEvent;
            packetEvent.Create(false);
            Check(client->Initialize(AUDCLNT_SHAREMODE_SHARED,
                AUDCLNT_STREAMFLAGS_LOOPBACK | AUDCLNT_STREAMFLAGS_EVENTCALLBACK,
                0, 0, mixFormat.value, nullptr), "Initialize endpoint loopback");
            ComPtr<IAudioCaptureClient> reader;
            Check(client->GetService(IID_PPV_ARGS(&reader)), "Get endpoint capture service");
            Check(client->SetEventHandle(packetEvent.value), "Set endpoint capture event");
            StopClient stopClient{client.Get()};
            Check(client->Start(), "Start endpoint capture");
            {
                std::lock_guard lock(mutex);
                endpointId = Utf8(id.value);
                sampleRate = format.rate;
                formatReport = format.Report();
                stats = Stats(format.channels);
                running = ready = startupComplete = true;
            }
            changed.notify_all();

            const auto drain = [&](bool discard) {
                for (;;) {
                    UINT32 available{};
                    Check(reader->GetNextPacketSize(&available), "Get endpoint packet size");
                    if (!available) return;
                    BYTE* bytes{};
                    UINT32 count{};
                    DWORD flags{};
                    UINT64 position{}, qpc{};
                    Check(reader->GetBuffer(&bytes, &count, &flags, &position, &qpc),
                        "Get endpoint packet");
                    try {
                        if (!discard) {
                            std::lock_guard lock(mutex);
                            stats.Add(bytes, count, flags, qpc, format, toneHz);
                        }
                    } catch (...) {
                        reader->ReleaseBuffer(count);
                        throw;
                    }
                    Check(reader->ReleaseBuffer(count), "Release endpoint packet");
                    if (WaitForSingleObject(stopEvent.value, 0) == WAIT_OBJECT_0) return;
                }
            };

            HANDLE events[]{stopEvent.value, resetEvent.value, packetEvent.value};
            for (;;) {
                const DWORD result = WaitForMultipleObjects(3, events, FALSE, 100);
                if (result == WAIT_OBJECT_0) break;
                if (result == WAIT_OBJECT_0 + 1) {
                    drain(true);
                    {
                        std::lock_guard lock(mutex);
                        stats = Stats(format.channels);
                        generation = requestedGeneration;
                    }
                    changed.notify_all();
                } else if (result == WAIT_OBJECT_0 + 2 || result == WAIT_TIMEOUT) {
                    drain(false);
                } else {
                    throw std::runtime_error("Waiting for endpoint capture failed: " +
                        std::to_string(GetLastError()));
                }
            }
            {
                std::lock_guard lock(mutex);
                running = false;
                ended = Clock::now();
            }
            changed.notify_all();
        } catch (const std::exception& exception) {
            Fail(exception.what());
        } catch (...) {
            Fail("Unknown endpoint capture failure");
        }
    }

    void StopLocked()
    {
        if (stopEvent.value) SetEvent(stopEvent.value);
        if (worker.joinable()) worker.join();
    }
};

AudioMeter::AudioMeter(double toneHz) : impl_(std::make_unique<Impl>(toneHz)) {}
AudioMeter::~AudioMeter() { Stop(); }

bool AudioMeter::Start()
{
    std::lock_guard lifecycleLock(impl_->lifecycleMutex);
    {
        std::lock_guard lock(impl_->mutex);
        if (impl_->running) return true;
    }
    impl_->StopLocked();
    {
        std::lock_guard lock(impl_->mutex);
        impl_->startupComplete = impl_->ready = impl_->running = false;
        impl_->generation = impl_->requestedGeneration = 0;
        impl_->sampleRate = 0;
        impl_->endpointId.clear();
        impl_->error.clear();
        impl_->formatReport = nullptr;
        impl_->stats = Stats();
        impl_->ended = impl_->stats.started;
    }
    try {
        impl_->stopEvent.Create(true);
        impl_->resetEvent.Create(false);
        impl_->worker = std::thread([this] { impl_->Run(); });
    } catch (const std::exception& exception) {
        impl_->Fail(exception.what());
        return false;
    }
    std::unique_lock lock(impl_->mutex);
    if (!impl_->changed.wait_for(lock, std::chrono::seconds(5),
        [&] { return impl_->startupComplete; })) {
        impl_->error = "Endpoint capture initialization timed out";
        lock.unlock();
        impl_->StopLocked();
        return false;
    }
    return impl_->ready && impl_->running && impl_->error.empty();
}

bool AudioMeter::Reset()
{
    std::lock_guard lifecycleLock(impl_->lifecycleMutex);
    std::unique_lock lock(impl_->mutex);
    if (!impl_->running || !impl_->error.empty()) return false;
    const auto requested = ++impl_->requestedGeneration;
    if (!SetEvent(impl_->resetEvent.value)) {
        impl_->error = "Signaling endpoint phase reset failed";
        return false;
    }
    if (!impl_->changed.wait_for(lock, std::chrono::seconds(3),
        [&] { return impl_->generation >= requested || !impl_->running; })) {
        impl_->error = "Endpoint phase reset timed out";
        return false;
    }
    return impl_->running && impl_->generation >= requested && impl_->error.empty();
}

json AudioMeter::Snapshot() const
{
    std::lock_guard lock(impl_->mutex);
    auto report = impl_->stats.Report(impl_->sampleRate, impl_->toneHz,
        impl_->running ? Clock::now() : impl_->ended);
    report["endpointId"] = impl_->endpointId;
    report["endpointRole"] = "default-console-render";
    report["format"] = impl_->formatReport;
    report["ready"] = impl_->ready;
    report["running"] = impl_->running;
    report["phaseGeneration"] = impl_->generation;
    report["error"] = impl_->error.empty() ? json(nullptr) : json(impl_->error);
    report["scope"] = "aggregate-endpoint-measurements-no-pcm-retained";
    return report;
}

void AudioMeter::Stop()
{
    std::lock_guard lifecycleLock(impl_->lifecycleMutex);
    impl_->StopLocked();
}
