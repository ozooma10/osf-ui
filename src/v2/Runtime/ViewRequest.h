#pragma once

#include <string>

namespace Runtime
{
    enum class ViewRequestAction
    {
        Open,
        Close
    };

    struct ViewRequest
    {
        ViewRequestAction action;
        std::string viewId;
    };
}