#include "SpatialAudio.h"
#include <windows.h>
#include <xaudio2.h>
#include <x3daudio.h>
#include <wrl.h>
#include <array>
#include <atomic>
#include <thread>

namespace osfui::cefhost
{
    namespace
    {
        using namespace browser::audio;
        constexpr unsigned kRate=48000, kFrames=480, kBlocks=4;
        struct Block { std::array<float,kFrames> samples{}; unsigned frames{}; };
        void Check(HRESULT hr, const char* operation)
        { if (FAILED(hr)) throw std::runtime_error(std::string(operation)+": "+std::to_string(hr)); }
        X3DAUDIO_VECTOR Vector(AudioVector v) { return {v.x,v.z,v.y}; }
        struct WakeEvent
        {
            HANDLE handle{CreateEventW(nullptr,FALSE,FALSE,nullptr)};
            WakeEvent() { if (!handle) Check(HRESULT_FROM_WIN32(GetLastError()),"Create audio wake event"); }
            ~WakeEvent() { CloseHandle(handle); }
            void Notify() const noexcept { SetEvent(handle); }
            void Wait(DWORD timeout) const noexcept { WaitForSingleObject(handle,timeout); }
        };
        struct SpatialSnapshot
        {
            AudioScene scene;
            std::array<float,kMaxAudioEmitters> distances{};
            std::array<float,XAUDIO2_MAX_AUDIO_CHANNELS> matrix{};
            unsigned outputChannels{};
            DWORD mask{};
            bool captured{}, fresh{}, enabled{}, muted{};
        };
    }
    void AudioSceneState::Update(const nlohmann::json& value, std::uint64_t expiresMs)
    {
        const auto decoded=Decode(value);
        std::lock_guard lock(mutex);
        scene=decoded.value_or(AudioScene{});
        expires=decoded && Fresh(expiresMs,ClockMs()) ? expiresMs : 0;
    }
    struct SpatialAudio::Impl : IXAudio2VoiceCallback, IXAudio2EngineCallback
    {
        std::shared_ptr<AudioSceneState> scene;
        mutable std::mutex mutex;
        // An auto-reset event retains callbacks arriving just before a wait and
        // never takes the PCM mutex on an XAudio2 callback thread.
        WakeEvent wake;
        std::array<Block,kBlocks> incoming;
        unsigned head{}, count{}, channels{}, generation{};
        bool active{};
        std::uint64_t packets{}, frames{}, dropped{};
        double squares{}, peak{};
        std::string error;
        SpatialSnapshot output;
        struct Submitted { Block block; std::atomic_bool busy{}; };
        std::array<Submitted,kBlocks> submitted;
        std::atomic<HRESULT> failure{S_OK};
        std::jthread worker;
        explicit Impl(std::shared_ptr<AudioSceneState> s) : scene(std::move(s)),
            worker([this](std::stop_token stop) { Run(stop); }) {}
        ~Impl() { worker.request_stop(); wake.Notify(); worker.join(); }
        void STDMETHODCALLTYPE OnBufferEnd(void* context) override
        { static_cast<Submitted*>(context)->busy.store(false,std::memory_order_release); wake.Notify(); }
        void STDMETHODCALLTYPE OnVoiceError(void*, HRESULT hr) override { failure=hr; wake.Notify(); }
        void STDMETHODCALLTYPE OnCriticalError(HRESULT hr) override { failure=hr; wake.Notify(); }
        void STDMETHODCALLTYPE OnProcessingPassStart() override {}
        void STDMETHODCALLTYPE OnProcessingPassEnd() override {}
        void STDMETHODCALLTYPE OnVoiceProcessingPassStart(UINT32) override {}
        void STDMETHODCALLTYPE OnVoiceProcessingPassEnd() override {}
        void STDMETHODCALLTYPE OnStreamEnd() override {}
        void STDMETHODCALLTYPE OnBufferStart(void*) override {}
        void STDMETHODCALLTYPE OnLoopEnd(void*) override {}

        void Run(std::stop_token stop) noexcept
        {
            const auto com=CoInitializeEx(nullptr,COINIT_MULTITHREADED);
            Microsoft::WRL::ComPtr<IXAudio2> engine;
            IXAudio2MasteringVoice* master{};
            IXAudio2SourceVoice* source{};
            X3DAUDIO_HANDLE spatial{};
            DWORD mask{};
            unsigned outputChannels{}, seenGeneration=~0u;
            ULONGLONG retryAt{}, spatialAt{}, diagnosticAt{};
            std::array<float,XAUDIO2_MAX_AUDIO_CHANNELS> matrix{}, current{};
            const auto destroySource=[&] {
                // DestroyVoice joins callbacks before their buffer storage is reused.
                if (source) { source->DestroyVoice(); source=nullptr; }
                for (auto& item:submitted) item.busy=false;
                current.fill(0);
            };
            const auto destroy=[&] {
                destroySource();
                if (engine) engine->UnregisterForCallbacks(this);
                if (master) { master->DestroyVoice(); master=nullptr; }
                engine.Reset();
            };
            while (!stop.stop_requested()) {
                try {
                    bool live; unsigned version;
                    { std::lock_guard lock(mutex); live=active; version=generation; }
                    Check(failure.exchange(S_OK),"Audio device");
                    if (seenGeneration!=version) { destroySource(); seenGeneration=version; }
                    const auto now=GetTickCount64();
                    // Device activation can take hundreds of milliseconds.
                    // Prepare it when the reserved view is created, before CEF
                    // delivers its first PCM packet, so the bounded queue need
                    // not discard the beginning of playback during activation.
                    if (now>=retryAt && !engine) {
                        Check(com,"Audio COM");
                        Check(XAudio2Create(&engine),"XAudio2Create");
                        Check(engine->RegisterForCallbacks(this),"Register audio callback");
                        // Default virtual client follows Windows' default endpoint.
                        Check(engine->CreateMasteringVoice(&master),"Create mastering voice");
                        Check(master->GetChannelMask(&mask),"Audio channel mask");
                        XAUDIO2_VOICE_DETAILS details{}; master->GetVoiceDetails(&details);
                        outputChannels=details.InputChannels;
                        Check(X3DAudioInitialize(mask,X3DAUDIO_SPEED_OF_SOUND,spatial),"X3DAudioInitialize");
                    }
                    if (live && engine && !source) {
                        WAVEFORMATEX format{WAVE_FORMAT_IEEE_FLOAT,1,kRate,kRate*4,4,32,0};
                        Check(engine->CreateSourceVoice(&source,&format,0,1,this),"Create TV voice");
                        Check(source->SetVolume(0),"Initial silence");
                        Check(source->Start(),"Start TV voice");
                        spatialAt=0;
                        { std::lock_guard lock(mutex); error.clear(); }
                    }
                    if (source && now-spatialAt>=10) {
                        AudioScene snapshot; ULONGLONG expires; bool muted;
                        { std::lock_guard lock(scene->mutex); snapshot=scene->scene; expires=scene->expires; muted=scene->muted; }
                        const bool fresh=Fresh(expires,ClockMs());
                        const bool enabled=fresh && snapshot.count && !muted && snapshot.gain>0;
                        matrix.fill(0);
                        const bool diagnosticDue=now-diagnosticAt>=250;
                        std::array<float,kMaxAudioEmitters> distances{};
                        if (enabled) {
                            X3DAUDIO_LISTENER listener{};
                            listener.Position=Vector(snapshot.position);
                            listener.OrientFront=Vector(snapshot.front); listener.OrientTop=Vector(snapshot.up);
                            for (unsigned i=0; i<snapshot.count; ++i) {
                                X3DAUDIO_EMITTER emitter{};
                                emitter.Position=Vector(snapshot.emitters[i].position);
                                emitter.OrientFront={0,0,1}; emitter.OrientTop={0,1,0}; emitter.ChannelCount=1;
                                X3DAUDIO_DISTANCE_CURVE_POINT points[]{{0,1},{1/kRange,1},{1,0}};
                                X3DAUDIO_DISTANCE_CURVE curve{points,3};
                                emitter.pVolumeCurve=&curve; emitter.CurveDistanceScaler=kRange;
                                std::array<float,XAUDIO2_MAX_AUDIO_CHANNELS> coefficients{};
                                X3DAUDIO_DSP_SETTINGS dsp{};
                                dsp.SrcChannelCount=1; dsp.DstChannelCount=outputChannels;
                                dsp.pMatrixCoefficients=coefficients.data();
                                X3DAudioCalculate(spatial,&listener,&emitter,X3DAUDIO_CALCULATE_MATRIX,&dsp);
                                for (unsigned c=0;c<outputChannels;++c) matrix[c]+=coefficients[c];
                                if (diagnosticDue) distances[i]=dsp.EmitterToListenerDistance;
                            }
                            Normalize(matrix.data(),outputChannels);
                        }
                        // Smooth moving emitters; disappearance/mute/stale poses silence immediately.
                        for (unsigned c=0;c<outputChannels;++c)
                            current[c]=enabled ? current[c]+.35f*(matrix[c]-current[c]) : 0;
                        Check(source->SetOutputMatrix(master,1,outputChannels,current.data()),"TV spatial matrix");
                        Check(source->SetVolume(enabled ? snapshot.gain : 0),"TV gain");
                        if (diagnosticDue) {
                            const SpatialSnapshot state{snapshot,distances,current,outputChannels,mask,true,fresh,enabled,muted};
                            std::lock_guard lock(mutex); output=std::move(state); diagnosticAt=now;
                        }
                        spatialAt=now;
                    }
                    if (source) for (auto& item:submitted) {
                        if (item.busy.load(std::memory_order_acquire)) continue;
                        {
                            std::lock_guard lock(mutex);
                            if (!count || generation!=seenGeneration) break;
                            item.block=incoming[head]; head=(head+1)%kBlocks; --count;
                        }
                        XAUDIO2_BUFFER buffer{};
                        buffer.AudioBytes=item.block.frames*sizeof(float);
                        buffer.pAudioData=reinterpret_cast<const BYTE*>(item.block.samples.data()); buffer.pContext=&item;
                        item.busy.store(true,std::memory_order_release);
                        Check(source->SubmitSourceBuffer(&buffer),"Submit TV PCM");
                    }
                } catch (const std::exception& e) {
                    destroy(); retryAt=GetTickCount64()+1000;
                    std::lock_guard lock(mutex); error=e.what(); head=count=0; output={};
                } catch (...) {
                    destroy(); retryAt=GetTickCount64()+1000;
                    std::lock_guard lock(mutex); head=count=0; output={};
                }
                // Idle routes have no spatial work. Wake for stream/device/buffer
                // events, retry deadlines, or the next 10 ms active spatial update.
                // Active updates remain periodic so stale poses silence promptly
                // even when the game stops sending scenes.
                const auto now=GetTickCount64();
                const auto deadline=engine ? spatialAt+10 : retryAt;
                const DWORD timeout=engine && !source ? INFINITE :
                    static_cast<DWORD>(deadline>now ? deadline-now : 0);
                wake.Wait(timeout);
            }
            destroy();
            if (SUCCEEDED(com)) CoUninitialize();
        }
    };
    SpatialAudio::SpatialAudio(std::shared_ptr<AudioSceneState> scene) : impl_(std::make_unique<Impl>(std::move(scene))) {}
    SpatialAudio::~SpatialAudio()=default;
    void SpatialAudio::Start(int rate, int channels) noexcept
    {
        auto& p=*impl_; std::lock_guard lock(p.mutex);
        p.active=rate==kRate && (channels==1 || channels==2); p.channels=channels;
        p.head=p.count=0; ++p.generation; p.wake.Notify();
    }
    void SpatialAudio::Packet(const float** samples, int frames) noexcept
    {
        auto& p=*impl_; std::lock_guard lock(p.mutex);
        if (!p.active || !samples || frames<=0 || frames>kFrames) { ++p.dropped; return; }
        for (unsigned c=0;c<p.channels;++c) if (!samples[c]) { ++p.dropped; return; }
        if (p.count==kBlocks) { p.head=(p.head+1)%kBlocks; --p.count; ++p.dropped; }
        auto& block=p.incoming[(p.head+p.count)%kBlocks]; block.frames=frames;
        for (int f=0; f<frames; ++f) {
            float value{};
            for (unsigned c=0;c<p.channels;++c) value+=samples[c][f]/p.channels;
            value=std::isfinite(value) ? std::clamp(value,-1.f,1.f) : 0;
            block.samples[f]=value; p.squares+=double(value)*value; p.peak=std::max(p.peak,double(std::abs(value)));
        }
        ++p.count; ++p.packets; p.frames+=frames; p.wake.Notify();
    }
    void SpatialAudio::Stop() noexcept
    { auto& p=*impl_; std::lock_guard lock(p.mutex); p.active=false; p.head=p.count=0; ++p.generation; p.wake.Notify(); }
    void SpatialAudio::Error(const std::string& text) noexcept
    { Stop(); try { std::lock_guard lock(impl_->mutex); impl_->error=text; } catch (...) {} }
    nlohmann::json SpatialAudio::Status() const
    {
        auto& p=*impl_;
        SpatialSnapshot output;
        bool active; unsigned generation;
        std::uint64_t packets, frames, dropped;
        double squares, peak;
        std::string error;
        {
            std::lock_guard lock(p.mutex);
            active=p.active; generation=p.generation; packets=p.packets; frames=p.frames;
            dropped=p.dropped; squares=p.squares; peak=p.peak; error=p.error; output=p.output;
        }
        // Serialization is developer-only and stays outside the PCM mutex.
        nlohmann::json spatial{{"enabled",false}};
        if (output.captured) {
            auto emitters=nlohmann::json::array();
            if (output.enabled) for (unsigned i=0; i<output.scene.count; ++i)
                emitters.push_back({{"id",output.scene.emitters[i].id},{"distance",output.distances[i]},
                    {"position",browser::audio::Vector(output.scene.emitters[i].position)}});
            spatial={{"fresh",output.fresh},{"enabled",output.enabled},{"muted",output.muted},{"gain",output.scene.gain},
                {"listener",browser::audio::Vector(output.scene.position)},{"front",browser::audio::Vector(output.scene.front)},
                {"up",browser::audio::Vector(output.scene.up)},{"emitters",std::move(emitters)},
                {"emitterCount",output.scene.count},{"matrix",std::vector<float>(output.matrix.begin(),output.matrix.begin()+output.outputChannels)},
                {"outputChannels",output.outputChannels},{"channelMask",output.mask}};
        }
        return {{"active",active},{"packets",packets},{"frames",frames},{"droppedPackets",dropped},
            {"inputEnergy",squares},{"inputRms",frames ? std::sqrt(squares/frames) : 0},
            {"peak",peak},{"generation",generation},{"error",error},{"spatial",std::move(spatial)}};
    }
}
