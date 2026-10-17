#pragma once
#include <cstdint>

namespace bench {
// Each opaque BGRA pixel encodes three nibbles, allowing at most two levels of
// channel conversion error. Zero remains the invalid marker sentinel.
inline std::uint32_t DecodeMarker(const std::uint8_t* pixels) {
    std::uint32_t value = 0;
    for (unsigned half = 0; half < 2; ++half) {
        const auto* pixel = pixels + half * 4;
        if (pixel[3] != 255) return 0;
        for (int channel = 2; channel >= 0; --channel) {
            const int sample = pixel[channel];
            const int nibble = sample / 16;
            const int error = sample - (8 + nibble * 16);
            if (error < -2 || error > 2) return 0;
            value = (value << 4) | nibble;
        }
    }
    return value;
}
}
