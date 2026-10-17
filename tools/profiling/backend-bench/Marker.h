#pragma once
#include <cstdint>

namespace bench {
// Eight opaque RGB blocks encode three bits each. Reject the middle third of
// each channel's range instead of depending on color-managed byte equality.
// Blocks occupy a 4x2 grid inside 32x32 pixels. Zero is the invalid sentinel.
inline std::uint32_t DecodeMarker(const std::uint8_t* pixels, unsigned rowPitch) {
    std::uint32_t value = 0;
    for (unsigned block = 0; block < 8; ++block) {
        const auto* pixel = pixels + (8 + (block / 4) * 16) * rowPitch + (4 + (block % 4) * 8) * 4;
        if (pixel[3] != 255) return 0;
        for (int channel = 2; channel >= 0; --channel) {
            const int sample = pixel[channel];
            if (sample > 85 && sample < 170) return 0;
            value = (value << 1) | (sample >= 170);
        }
    }
    return value;
}

inline bool ValidHudPixel(std::uint32_t marker, std::uint32_t pixel,
                          std::uint32_t bar, std::uint32_t background) {
    if (!pixel || !bar || !background || int((bar >> 8) & 255) - int((background >> 8) & 255) <= 80)
        return false;
    const auto expected = (marker & 1) ? background : bar;
    for (unsigned shift = 0; shift < 24; shift += 8) {
        const int difference = int((pixel >> shift) & 255) - int((expected >> shift) & 255);
        if (difference < -2 || difference > 2) return false;
    }
    return true;
}
}
