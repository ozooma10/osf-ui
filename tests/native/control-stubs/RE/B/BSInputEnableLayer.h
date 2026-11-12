#pragma once
#include "RE/U/UserEvents.h"
namespace RE {
struct BSInputEnableLayer {
    std::uint32_t user=~0u, other=~0u;
    unsigned writes=0;
    unsigned GetLayerID() const { return 7; }
    void EnableUserEvent(USER_EVENT_FLAG flags, bool enabled) {
        ++writes; if(enabled) user|=std::uint32_t(flags); else user&=~std::uint32_t(flags);
    }
    void EnableOtherEvent(OTHER_EVENT_FLAG flags, bool enabled) {
        ++writes; if(enabled) other|=std::uint32_t(flags); else other&=~std::uint32_t(flags);
    }
};
}
