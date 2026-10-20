#pragma once
#include <cstdint>

namespace OSFUI::API::Surfaces
{
    // ABI 1.2. Copied by value; no engine pointers cross the DLL/host boundary.
    inline constexpr std::uint32_t kMaxAudioEmitters = 32;
    struct AudioVector { float x{}, y{}, z{}; };
    struct AudioEmitter { std::uint32_t id{}; AudioVector position; };
    struct AudioScene
    {
        std::uint32_t size{sizeof(AudioScene)};
        // All positions use the same render origin, in metres, Z up.
        AudioVector position, front{0,1,0}, up{0,0,1};
        float gain{1};
        std::uint32_t count{};
        AudioEmitter emitters[kMaxAudioEmitters]{};
    };
}
