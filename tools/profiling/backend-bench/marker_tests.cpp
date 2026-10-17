#include "Marker.h"
#include <array>
#include <iostream>

int main() {
    constexpr unsigned pitch = 32 * 4 + 16; // Exercise GPU row padding too.
    for (std::uint32_t serial : {0x110001u, 0x1100d5u, 0x1100ffu, 0x110100u,
                                0x120001u, 0x130001u, 0x170001u, 0x180001u,
                                0x190001u, 0xffffffu}) {
        std::array<std::uint8_t, pitch * 32> pixels{};
        for (unsigned block = 0; block < 8; ++block) {
            const auto value = serial >> ((7 - block) * 3);
            auto* pixel = pixels.data() + (8 + block / 4 * 16) * pitch + (4 + block % 4 * 8) * 4;
            for (unsigned channel = 0; channel < 3; ++channel)
                pixel[channel] = (value & (1u << channel)) ? 255 : 0;
            pixel[3] = 255;
        }
        if (bench::DecodeMarker(pixels.data(), pitch) != serial) return 1;
        for (unsigned block = 0; block < 8; ++block) {
            const unsigned offset = (8 + block / 4 * 16) * pitch + (4 + block % 4 * 8) * 4;
            for (unsigned channel = 0; channel < 3; ++channel) {
                for (unsigned shift : {1u, 2u, 41u, 85u}) {
                    auto converted = pixels;
                    converted[offset + channel] = pixels[offset + channel] ? 255 - shift : shift;
                    if (bench::DecodeMarker(converted.data(), pitch) != serial) return 2;
                }
                for (unsigned invalid : {86u, 128u, 169u}) {
                    auto corrupt = pixels;
                    corrupt[offset + channel] = invalid;
                    if (bench::DecodeMarker(corrupt.data(), pitch) != 0) return 3;
                }
            }
            auto transparent = pixels;
            transparent[offset + 3] = 254;
            if (bench::DecodeMarker(transparent.data(), pitch) != 0) return 4;
        }
    }
    // Actual WGC-converted colors from the retained failing mixed-view capture.
    constexpr std::uint32_t bar = 0x25a35d, background = 0x101b28;
    if (!bench::ValidHudPixel(0x120002, bar, bar, background) ||
        !bench::ValidHudPixel(0x120003, background, bar, background) ||
        !bench::ValidHudPixel(0x120003, background + 2, bar, background)) return 5;
    if (bench::ValidHudPixel(0x120003, bar, bar, background) || // Stale bar after shrinking.
        bench::ValidHudPixel(0x120002, background, bar, background) || // Missing expanded bar.
        bench::ValidHudPixel(0x120003, background, background, background) || // Entire HUD absent.
        bench::ValidHudPixel(0x120003, background + 3, bar, background) ||
        bench::ValidHudPixel(0x120003, 0, bar, background)) return 6;
    std::cout << "Marker conversion, row padding, invalid pixels and HUD erase checks passed\n";
}
