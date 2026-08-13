#pragma once

#include <string>

namespace Runtime
{
    enum class ViewRequestAction
    {
        Open,
        Close,
        CloseAll
    };

    enum class ViewRequestResult
    {
        Accepted,
        InvalidViewId,
        UnknownView,
        NotInstantiated,
        BlockedByGameMenu,
        InputUnavailable,
        QueueFull,
        Unavailable
    };

    struct ViewRequest
    {
        ViewRequestAction action;
        std::string viewId;
    };
}
