#pragma once
#include "../../sdk/OSFUI_SurfaceAudio.h"
#include <nlohmann/json.hpp>
#include <algorithm>
#include <cmath>
#include <chrono>
#include <optional>

namespace osfui::browser::audio
{
    using namespace OSFUI::API::Surfaces;
    inline constexpr unsigned kSceneTimeoutMs = 1000;
    inline constexpr float kRange = 20;
    // Both Windows processes use steady_clock's system-wide QPC epoch. Carry an
    // absolute deadline so delayed pipe/UI dispatch cannot revive an old scene.
    inline std::uint64_t ClockMs()
    { return std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now().time_since_epoch()).count(); }
    inline bool Fresh(std::uint64_t deadline, std::uint64_t now)
    { return deadline>now && deadline-now<=kSceneTimeoutMs; }
    inline float Dot(AudioVector a, AudioVector b) { return a.x*b.x+a.y*b.y+a.z*b.z; }
    inline float DistanceSquared(AudioVector a, AudioVector b)
    { return Dot({a.x-b.x,a.y-b.y,a.z-b.z},{a.x-b.x,a.y-b.y,a.z-b.z}); }
    inline bool Finite(AudioVector v)
    { return std::isfinite(v.x) && std::isfinite(v.y) && std::isfinite(v.z); }
    inline bool Valid(const AudioScene& s)
    {
        if (s.size < sizeof(AudioScene) || s.count > kMaxAudioEmitters ||
            !Finite(s.position) || !Finite(s.front) || !Finite(s.up) ||
            !std::isfinite(s.gain) || s.gain < 0 || s.gain > 1 ||
            std::abs(Dot(s.front,s.front)-1) > .02f ||
            std::abs(Dot(s.up,s.up)-1) > .02f || std::abs(Dot(s.front,s.up)) > .02f) return false;
        for (unsigned i=0; i<s.count; ++i) {
            if (!s.emitters[i].id || !Finite(s.emitters[i].position)) return false;
            for (unsigned j=0; j<i; ++j) if (s.emitters[i].id == s.emitters[j].id) return false;
        }
        return true;
    }
    inline nlohmann::json Vector(AudioVector v) { return {v.x,v.y,v.z}; }
    inline nlohmann::json Encode(const AudioScene& s)
    {
        auto emitters=nlohmann::json::array();
        for (unsigned i=0; i<s.count; ++i)
            emitters.push_back({{"id",s.emitters[i].id},{"position",Vector(s.emitters[i].position)}});
        return {{"position",Vector(s.position)},{"front",Vector(s.front)},{"up",Vector(s.up)},
            {"gain",s.gain},{"emitters",std::move(emitters)}};
    }
    inline std::optional<AudioScene> Decode(const nlohmann::json& value)
    {
        try {
            AudioScene s;
            const auto vector=[](const nlohmann::json& v) {
                if (!v.is_array() || v.size()!=3) throw std::runtime_error("audio vector");
                return AudioVector{v.at(0).get<float>(),v.at(1).get<float>(),v.at(2).get<float>()};
            };
            s.position=vector(value.at("position")); s.front=vector(value.at("front")); s.up=vector(value.at("up"));
            s.gain=value.at("gain").get<float>();
            const auto& emitters=value.at("emitters");
            if (!emitters.is_array() || emitters.size()>kMaxAudioEmitters) return {};
            for (const auto& e:emitters) {
                const auto& id=e.at("id");
                if (!id.is_number_integer() || id.get<double>()<=0 || id.get<double>()>UINT32_MAX) return {};
                s.emitters[s.count++]={id.get<std::uint32_t>(),vector(e.at("position"))};
            }
            return Valid(s) ? std::optional(s) : std::nullopt;
        } catch (...) { return {}; }
    }
    // Identical PCM is emitted by every TV. Summing their spatial matrices before
    // playback is equivalent to synchronized mono voices, without N copies of PCM.
    // Preserve single-TV level and cap coherent summation when TVs are clustered.
    inline void Normalize(float* matrix, unsigned channels)
    {
        float sum{};
        for (unsigned i=0; i<channels; ++i) sum+=matrix[i]*matrix[i];
        for (unsigned i=0; i<channels; ++i) matrix[i]/=std::max(1.f,std::sqrt(sum));
    }
}
