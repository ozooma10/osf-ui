#include "ViewStartupPolicy.h"

namespace Runtime
{
    bool ShouldAutoStartDiscoveredView(const ViewManifest& a_view, const DiscoveredViewStartupContext& a_context)
    {
        if (a_view.kind != ViewKind::Hud || !a_view.catalogVisible) {
            return false;
        }

        return a_context.playerOverride.value_or(a_view.openOnStart);
    }
}
