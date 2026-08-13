#pragma once

#include "ViewManifest.h"

namespace Runtime
{
    enum class ViewPresentationAction
    {
        Show,
        Hide
    };

    struct ViewPresentationCommand
    {
        ViewPresentationAction action{ ViewPresentationAction::Show };
        ViewManifest view;
    };
}
