#pragma once

#include "IViewMessageTransport.h"
#include "v2/Runtime/ViewRequest.h"

#include <functional>
#include <memory>
#include <string>
#include <string_view>
#include <unordered_set>

namespace OSFUI
{
    class MessageBridge;
}

namespace Bridge
{
    class BridgeRuntime
    {
    public:
        using RequestViewHandler = std::function<Runtime::ViewRequestResult(Runtime::ViewRequestAction a_action, std::string a_viewId)>;

        BridgeRuntime(IViewMessageTransport& a_transport, RequestViewHandler a_requestView);
        ~BridgeRuntime();

        BridgeRuntime(const BridgeRuntime&) = delete;
        BridgeRuntime& operator=(const BridgeRuntime&) = delete;

        void OnDocumentCreated(std::string_view a_viewId);
        void OnDocumentDestroyed(std::string_view a_viewId);
        void ResetDocuments();
        void Tick();

    private:
        void RegisterEndpoints();
        void HandleWebMessage(std::string_view a_viewId, std::string_view a_json);
        void RequestView(Runtime::ViewRequestAction a_action, std::string a_viewId, OSFUI::MessageBridge& a_bridge);

        IViewMessageTransport& _transport;
        RequestViewHandler _requestView;
        std::unique_ptr<OSFUI::MessageBridge> _bridge;
        std::unordered_set<std::string> _documents;
        std::unordered_set<std::string> _warnedUnknownSources;
    };
}
