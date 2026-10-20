#pragma once
#include "BrowserAudio.h"
#include <memory>
#include <mutex>
#include <string>

namespace osfui::cefhost
{
    struct AudioSceneState
    {
        std::mutex mutex;
        browser::audio::AudioScene scene;
        unsigned long long expires{};
        bool muted{};
        void Update(const nlohmann::json&, std::uint64_t expiresMs);
    };
    class SpatialAudio
    {
    public:
        explicit SpatialAudio(std::shared_ptr<AudioSceneState>);
        ~SpatialAudio();
        void Start(int rate, int channels) noexcept;
        void Packet(const float** samples, int frames) noexcept;
        void Stop() noexcept;
        void Error(const std::string&) noexcept;
        nlohmann::json Status() const;
    private:
        struct Impl;
        std::unique_ptr<Impl> impl_;
    };
}
