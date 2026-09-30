#pragma once
#include "RE/B/BSScriptUtil.h"

namespace RE
{
    enum class UI_MESSAGE_TYPE : std::uint32_t { kShow = 0, kUpdate = 1, kHide = 2 };
    class UIMessageQueue
    {
    public:
        static UIMessageQueue* GetSingleton() { return nullptr; }
        std::int64_t AddMessage(const BSFixedString&, UI_MESSAGE_TYPE) { return 0; }
    };
}
