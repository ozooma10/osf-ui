#pragma once
#include <string>
#include <vector>

namespace OSFUI::ScreenBindingRuntime
{
    // Opt-in build. Content declares ACTI bases using the supported screen model.
    void Install();
    void OnDataLoaded();
    void Configure(std::vector<std::string> views);
    bool Enabled();
}
