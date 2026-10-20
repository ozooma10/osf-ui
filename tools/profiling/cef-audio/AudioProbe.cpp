#include "AudioProbe.h"
#include <windows.h>
#include <xaudio2.h>
#include <x3daudio.h>
#include <wrl.h>
#include <algorithm>
#include <array>
#include <atomic>
#include <cmath>
#include <fstream>
#include <numbers>
#include <thread>

namespace osfui::cefhost::probe {
using json = nlohmann::json;
namespace {
constexpr int kRate = 48000, kFrames = 2048, kBlocks = 8;
struct Block { std::array<float, kFrames> samples{}; unsigned frames{}; };
void Check(HRESULT hr, const char* where) {
    if (FAILED(hr)) throw std::runtime_error(std::string(where) + ": " + std::to_string(hr));
}
X3DAUDIO_VECTOR Vector(const json& a, int offset) {
    // Starfield: X right, Y forward, Z up. X3DAudio: X right, Y up, Z forward.
    return {a.at(offset).get<float>(), a.at(offset + 2).get<float>(), a.at(offset + 1).get<float>()};
}
}
void Scene::Update(const json& args) {
    if (!args.is_array() || args.size() != 9) return;
    for (const auto& value : args)
        if (!value.is_number() || !std::isfinite(value.get<double>())) return;
    std::lock_guard lock(mutex);
    pose = args;
    updated = GetTickCount64();
}

struct Audio::Impl : IXAudio2VoiceCallback {
    std::shared_ptr<Scene> scene;
    std::filesystem::path evidence;
    std::mutex mutex;
    std::array<Block, kBlocks> incoming;
    unsigned head{}, count{}, channels{}, generation{};
    bool active{};
    std::uint64_t packets{}, frames{}, dropped{};
    double squares{}, peak{};
    std::string error;
    struct Submitted { Block block; std::atomic_bool busy{}; };
    std::array<Submitted, kBlocks> submitted;
    std::atomic<HRESULT> voiceError{S_OK};
    std::jthread worker;
    Impl(std::shared_ptr<Scene> s, std::filesystem::path p) : scene(std::move(s)), evidence(std::move(p)) {
        worker = std::jthread([this](std::stop_token stop) { Run(stop); });
    }
    ~Impl() { worker.request_stop(); worker.join(); }
    void STDMETHODCALLTYPE OnBufferEnd(void* context) override {
        static_cast<Submitted*>(context)->busy.store(false, std::memory_order_release);
    }
    void STDMETHODCALLTYPE OnVoiceError(void*, HRESULT hr) override { voiceError.store(hr); }
    void STDMETHODCALLTYPE OnVoiceProcessingPassStart(UINT32) override {}
    void STDMETHODCALLTYPE OnVoiceProcessingPassEnd() override {}
    void STDMETHODCALLTYPE OnStreamEnd() override {}
    void STDMETHODCALLTYPE OnBufferStart(void*) override {}
    void STDMETHODCALLTYPE OnLoopEnd(void*) override {}
    void Write(json value) {
        value["tickMs"] = GetTickCount64();
        const auto temp = evidence.wstring() + L".tmp";
        { std::ofstream stream(temp); stream << value.dump(2); }
        MoveFileExW(temp.c_str(), evidence.c_str(), MOVEFILE_REPLACE_EXISTING);
    }
    void Run(std::stop_token stop) noexcept {
        Microsoft::WRL::ComPtr<IXAudio2> engine;
        IXAudio2MasteringVoice* master{};
        IXAudio2SourceVoice* source{};
        bool com = false;
        try {
            Check(CoInitializeEx(nullptr, COINIT_MULTITHREADED), "COM"); com = true;
            Check(XAudio2Create(&engine), "XAudio2Create");
            Check(engine->CreateMasteringVoice(&master), "CreateMasteringVoice");
            DWORD mask{}; Check(master->GetChannelMask(&mask), "GetChannelMask");
            XAUDIO2_VOICE_DETAILS details{}; master->GetVoiceDetails(&details);
            X3DAUDIO_HANDLE spatial;
            Check(X3DAudioInitialize(mask, X3DAUDIO_SPEED_OF_SOUND, spatial), "X3DAudioInitialize");
            std::vector<float> matrix(details.InputChannels);
            WAVEFORMATEX format{WAVE_FORMAT_IEEE_FLOAT, 1, kRate, kRate * 4, 4, 32, 0};
            unsigned seenGeneration = ~0u;
            ULONGLONG written{}, spatialAt{};
            json spatialState;
            while (!stop.stop_requested()) {
                bool live; unsigned currentGeneration;
                {
                    std::lock_guard lock(mutex);
                    live = active; currentGeneration = generation;
                }
                if (currentGeneration != seenGeneration) {
                    if (source) { source->DestroyVoice(); source = nullptr; }
                    for (auto& item : submitted) item.busy.store(false);
                    if (live) {
                        Check(engine->CreateSourceVoice(&source, &format, 0, 1, this), "CreateSourceVoice");
                        Check(source->SetVolume(0), "Initial silence");
                        Check(source->Start(), "Start source");
                    }
                    seenGeneration = currentGeneration;
                }
                Check(voiceError.load(), "Voice callback");
                const auto now = GetTickCount64();
                if (source && now - spatialAt >= 20) {
                    json pose; ULONGLONG updated; bool muted, fullscreen;
                    { std::lock_guard lock(scene->mutex); pose = scene->pose; updated = scene->updated;
                      muted = scene->muted || !scene->enabled; fullscreen = scene->fullscreen; }
                    // Watch deliberately pauses the VM. Retain the last known
                    // pose during this pause; ordinary stale world poses fail silent.
                    const bool fresh = updated && now - updated < 2000;
                    if (fresh || (fullscreen && updated)) {
                        X3DAUDIO_LISTENER listener{};
                        listener.Position = Vector(pose, 0);
                        const float yaw = pose[3].get<float>() * std::numbers::pi_v<float> / 180;
                        listener.OrientFront = {std::sin(yaw), 0, std::cos(yaw)};
                        listener.OrientTop = {0, 1, 0};
                        X3DAUDIO_EMITTER emitter{};
                        emitter.Position = Vector(pose, 4);
                        emitter.OrientFront = {0,0,1}; emitter.OrientTop = {0,1,0};
                        emitter.ChannelCount = 1;
                        // Deliberately simple prototype rolloff: full at 1 m, zero at 20 m.
                        X3DAUDIO_DISTANCE_CURVE_POINT points[]{{0,1},{0.05f,1},{1,0}};
                        X3DAUDIO_DISTANCE_CURVE curve{points,3};
                        emitter.pVolumeCurve = &curve; emitter.CurveDistanceScaler = 20;
                        X3DAUDIO_DSP_SETTINGS dsp{};
                        dsp.SrcChannelCount = 1; dsp.DstChannelCount = details.InputChannels;
                        dsp.pMatrixCoefficients = matrix.data();
                        X3DAudioCalculate(spatial, &listener, &emitter, X3DAUDIO_CALCULATE_MATRIX, &dsp);
                        Check(source->SetOutputMatrix(master, 1, details.InputChannels, matrix.data()), "Spatial matrix");
                        Check(source->SetVolume(muted || pose[7].get<float>() == 0 ? 0 : 1), "Source volume");
                        spatialState = {{"pose",pose},{"distance",dsp.EmitterToListenerDistance},
                            {"matrix",matrix},{"outputEnabled",!muted && pose[7].get<float>() != 0}};
                    } else { Check(source->SetVolume(0), "Stale pose silence"); spatialState = {{"outputEnabled",false}}; }
                    spatialState["poseFresh"] = fresh; spatialState["poseTickMs"] = updated;
                    spatialState["fullscreen"] = fullscreen;
                    spatialState["poseFrozen"] = fullscreen && updated && !fresh;
                    spatialAt = now;
                }
                if (source) {
                    for (auto& item : submitted) {
                        if (item.busy.load(std::memory_order_acquire)) continue;
                        {
                            std::lock_guard lock(mutex);
                            if (!count || generation != seenGeneration) break;
                            item.block = incoming[head]; head = (head + 1) % kBlocks; --count;
                        }
                        XAUDIO2_BUFFER buffer{};
                        buffer.AudioBytes = item.block.frames * sizeof(float);
                        buffer.pAudioData = reinterpret_cast<const BYTE*>(item.block.samples.data());
                        buffer.pContext = &item;
                        item.busy.store(true, std::memory_order_release);
                        Check(source->SubmitSourceBuffer(&buffer), "Submit PCM");
                    }
                }
                if (now - written >= 250) {
                    json value;
                    {
                        std::lock_guard lock(mutex);
                        value = {{"active",active},{"packets",packets},{"frames",frames},{"droppedPackets",dropped},
                            {"inputRms",frames ? std::sqrt(squares / frames) : 0},{"inputEnergy",squares},{"inputPeak",peak},
                            {"rate",kRate},{"channels",channels},{"error",error},{"generation",generation}};
                    }
                    value["spatial"] = spatialState; value["outputChannels"] = details.InputChannels;
                    value["channelMask"] = mask; value["pid"] = GetCurrentProcessId();
                    Write(std::move(value)); written = now;
                }
                Sleep(5);
            }
        } catch (const std::exception& e) {
            try { Write({{"fatal",e.what()},{"pid",GetCurrentProcessId()}}); } catch (...) {}
        }
        // DestroyVoice waits for buffer callbacks before submitted storage is released.
        if (source) source->DestroyVoice();
        if (master) master->DestroyVoice();
        engine.Reset();
        if (com) CoUninitialize();
    }
};

Audio::Audio(std::shared_ptr<Scene> scene, std::filesystem::path evidence) :
    impl_(std::make_unique<Impl>(std::move(scene), std::move(evidence))) {}
Audio::~Audio() = default;
void Audio::Start(int rate, int channels) {
    auto& p = *impl_; std::lock_guard lock(p.mutex);
    p.active = rate == kRate && (channels == 1 || channels == 2);
    p.channels = channels; p.head = p.count = 0; ++p.generation;
    if (!p.active) p.error = "Unsupported CEF PCM format";
}
void Audio::Packet(const float** samples, int frames) {
    auto& p = *impl_; std::lock_guard lock(p.mutex);
    if (!p.active || !samples || frames <= 0 || frames > kFrames) { ++p.dropped; return; }
    if (p.count == kBlocks) { p.head = (p.head + 1) % kBlocks; --p.count; ++p.dropped; }
    auto& block = p.incoming[(p.head + p.count) % kBlocks];
    block.frames = frames;
    for (int f = 0; f < frames; ++f) {
        float sample = 0;
        for (unsigned c = 0; c < p.channels; ++c) sample += samples[c][f] / p.channels;
        if (!std::isfinite(sample)) sample = 0;
        block.samples[f] = std::clamp(sample, -1.f, 1.f);
        p.squares += double(sample) * sample; p.peak = std::max(p.peak, double(std::abs(sample)));
    }
    ++p.count; ++p.packets; p.frames += frames;
}
void Audio::Stop() {
    auto& p = *impl_; std::lock_guard lock(p.mutex);
    p.active = false; p.head = p.count = 0; ++p.generation;
}
void Audio::Error(const std::string& message) { Stop(); std::lock_guard lock(impl_->mutex); impl_->error = message; }
}
