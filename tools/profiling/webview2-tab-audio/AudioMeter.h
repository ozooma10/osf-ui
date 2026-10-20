#pragma once

#include <memory>
#include <nlohmann/json_fwd.hpp>

// Read-only default-render-endpoint loopback meter for the standalone probe.
// Captures aggregate measurements only. It never changes volume or saves PCM.
class AudioMeter final {
public:
    explicit AudioMeter(double toneHz = 997.0);
    ~AudioMeter();

    AudioMeter(const AudioMeter&) = delete;
    AudioMeter& operator=(const AudioMeter&) = delete;

    // Waits for capture initialization. Errors are retained in Snapshot().
    bool Start();

    // Discards queued packets and starts a fresh measurement phase on the
    // capture thread. Returns after acknowledgment, or false on failure.
    bool Reset();

    // Thread-safe cumulative measurements since the last successful Reset().
    // channelToneRms is a narrow-band estimate, not source attribution.
    nlohmann::json Snapshot() const;

    // Joins the worker and retains its final measurements.
    void Stop();

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};
