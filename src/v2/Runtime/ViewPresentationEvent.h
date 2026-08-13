#pragma once

#include <string>

namespace Runtime
{
    enum class ViewPresentationEventKind
    {
        NotReady,
        Ready,
        ViewFailed,
        PresenterFailed
    };

    struct ViewPresentationEvent
    {
        ViewPresentationEventKind kind;
        std::string viewId;
        std::string detail;
    };
}
