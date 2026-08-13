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

    struct ViewRequest
    {
        ViewRequestAction action;
        std::string viewId;
    };
}
