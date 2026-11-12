#pragma once
#include "RE/B/BSInputEnableLayer.h"
namespace RE {
struct BSInputEnableManager {
    inline static bool available=false;
    inline static BSInputEnableLayer owned, foreign;
    static BSInputEnableManager* GetSingleton() { static BSInputEnableManager instance; return available?&instance:nullptr; }
    bool AllocateNewLayer(BSInputEnableLayer** layer, const char*) { *layer=&owned; return true; }
};
}
