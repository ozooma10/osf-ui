#pragma once
// Private, compile-time CEF feasibility experiment. Never part of a release build.
#include <nlohmann/json.hpp>
#include <filesystem>
#include <memory>
#include <mutex>

namespace osfui::cefhost::probe {
struct Scene {
    std::mutex mutex;
    nlohmann::json pose;
    unsigned long long updated{};
    bool muted{}, enabled{true}, fullscreen{};
    void Update(const nlohmann::json& args);
};

class Audio {
public:
    Audio(std::shared_ptr<Scene>, std::filesystem::path evidence);
    ~Audio();
    void Start(int rate, int channels);
    void Packet(const float** samples, int frames);
    void Stop();
    void Error(const std::string&);
private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};
}
