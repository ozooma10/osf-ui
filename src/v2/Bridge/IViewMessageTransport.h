#pragma once

#include <functional>
#include <string_view>

namespace Bridge
{
    class IViewMessageTransport
    {
    public:
        using WebMessageHandler = std::function<void(std::string_view a_viewId, std::string_view a_json)>;

        virtual ~IViewMessageTransport() = default;

        // Inbound messages are delivered from the presenter's main-thread Tick. Replacing the handler with an empty function unsubscribes.
        virtual void SetWebMessageHandler(WebMessageHandler a_handler) = 0;

        // Delivers one protocol envelope to a browser document. Implementations contain renderer failures and report them through presentation state.
        virtual void SendMessageToWeb(std::string_view a_viewId, std::string_view a_json) noexcept = 0;
    };
}
