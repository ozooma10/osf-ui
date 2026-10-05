#pragma once

#include <chrono>
#include <functional>
#include <string>
#include <string_view>
#include <vector>

namespace RE::BSScript { class IVirtualMachine; }
namespace OSFUI { class MessageBridge; }

namespace OSFUI::Compat::V1::Papyrus
{
    // Frozen OSFUI.psc natives.
    void BindNatives(RE::BSScript::IVirtualMachine& vm);
    // Any thread. Clear VM handles and reply tokens when the game session ends.
    void ResetSession();
    void SetSuspended(bool suspended);

    // Runtime thread only. The predicate admits only discovered, instantiated legacy views.
    void RegisterEndpoints(MessageBridge& bridge, std::function<bool(std::string_view)> admits);
    void Pump(MessageBridge& bridge, const std::vector<std::string>& views, bool allowHotkeys,
        std::chrono::steady_clock::time_point now = std::chrono::steady_clock::now());
    void ReplayState(MessageBridge& bridge, std::string_view view);
    void CloseView(MessageBridge& bridge, std::string_view view);
}
