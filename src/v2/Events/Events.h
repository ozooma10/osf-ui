#pragma once

#include <string_view>

namespace Events
{
    using MenuOpenCloseCallback = void (*)(std::string_view, bool);

    bool Register(MenuOpenCloseCallback a_callback);
}
