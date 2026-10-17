#include "Marker.h"
#include <array>
#include <iostream>

int main() {
    // Exercise every nibble value in every channel, including serial byte
    // boundaries and the layer-ID channels used by the consumer.
    for (unsigned place = 0; place < 6; ++place) {
        for (unsigned digit = 0; digit < 16; ++digit) {
            const std::uint32_t serial = (0x1200ffu & ~(15u << (place * 4))) | (digit << (place * 4));
            std::array<std::uint8_t, 8> pixels{};
            for (unsigned half = 0; half < 2; ++half) {
                const auto value = serial >> ((1 - half) * 12);
                for (unsigned channel = 0; channel < 3; ++channel)
                    pixels[half * 4 + channel] = 8 + ((value >> (channel * 4)) & 15) * 16;
                pixels[half * 4 + 3] = 255;
            }
            for (unsigned channel = 0; channel < 8; ++channel) {
                if (channel % 4 == 3) continue;
                for (int delta = -2; delta <= 2; ++delta) {
                    auto converted = pixels;
                    converted[channel] += delta;
                    if (bench::DecodeMarker(converted.data()) != serial) return 1;
                }
                for (int delta : {-3, 3}) {
                    auto corrupt = pixels;
                    corrupt[channel] += delta;
                    if (bench::DecodeMarker(corrupt.data()) != 0) return 2;
                }
            }
            for (unsigned alpha : {3u, 7u}) {
                auto transparent = pixels;
                transparent[alpha] = 254;
                if (bench::DecodeMarker(transparent.data()) != 0) return 3;
            }
        }
    }
    std::cout << "Marker channel conversion and invalid-pixel checks passed\n";
}
