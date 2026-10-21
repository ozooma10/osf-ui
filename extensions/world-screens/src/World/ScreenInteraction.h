#pragma once
#include "World/ScreenBindings.h"

namespace OSFUI::ScreenInteraction
{
    enum class Action { Wait, Forward, Keep, End, Reject };

    inline Action Decide(bool forwarded, bool cancelled, const std::optional<ScreenBindings::Selection>& selection,
        std::string_view view, bool expired)
    {
        const bool selected=!cancelled && selection && selection->view==view;
        // Readiness belongs to the selected reference. Another mirror can keep
        // its feed healthy after this material or geometry becomes unavailable.
        if (forwarded) return selected && selection->ready ? Action::Keep : Action::End;
        if (!selected || expired) return Action::Reject;
        return selection->ready ? Action::Forward : Action::Wait;
    }
}
