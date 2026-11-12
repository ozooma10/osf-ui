#pragma once
#include <cstdint>
namespace RE {
enum class USER_EVENT_FLAG : std::uint32_t {
    Movement=0x401, Looking=2, Activation=4, POVSwitch=32, Fighting=64, Sneaking=128, WheelZoom=512
};
enum class OTHER_EVENT_FLAG : std::uint32_t {
    Journal=1, Activate=2, FastTravel=4, VATS=16, Favorites=32, Running=256,
    Sprinting=1024, HandScanner=2048, Takeoff=4096, Inventory=8192, GravJump=16384, FarTravel=32768
};
constexpr USER_EVENT_FLAG operator|(USER_EVENT_FLAG a, USER_EVENT_FLAG b) { return USER_EVENT_FLAG(std::uint32_t(a)|std::uint32_t(b)); }
constexpr OTHER_EVENT_FLAG operator|(OTHER_EVENT_FLAG a, OTHER_EVENT_FLAG b) { return OTHER_EVENT_FLAG(std::uint32_t(a)|std::uint32_t(b)); }
}
