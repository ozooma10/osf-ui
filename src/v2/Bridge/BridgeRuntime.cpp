#include "BridgeRuntime.h"

#include "runtime/Json.h"
#include "runtime/MessageBridge.h"

namespace Bridge
{
    BridgeRuntime::BridgeRuntime(IViewMessageTransport& a_transport, RequestViewHandler a_requestView)
        : _transport(a_transport), _requestView(std::move(a_requestView)), _bridge(std::make_unique<OSFUI::MessageBridge>([this](std::string_view a_viewId, std::string_view a_json) { _transport.SendMessageToWeb(a_viewId, a_json); }))
    {
        RegisterEndpoints();
        _transport.SetWebMessageHandler([this](std::string_view a_viewId, std::string_view a_json) { HandleWebMessage(a_viewId, a_json); });
    }

    BridgeRuntime::~BridgeRuntime()
    {
        _transport.SetWebMessageHandler({});
    }

    void BridgeRuntime::OnDocumentCreated(std::string_view a_viewId)
    {
        const std::string viewId {a_viewId};
        _documents.insert(viewId);
        _warnedUnknownSources.erase(viewId);
        _bridge->OnViewCreated(viewId);
    }

    void BridgeRuntime::OnDocumentDestroyed(std::string_view a_viewId)
    {
        const std::string viewId {a_viewId};
        if (_documents.erase(viewId) != 0) {
            _bridge->OnViewDestroyed(viewId);
        }
        _warnedUnknownSources.erase(viewId);
    }

    void BridgeRuntime::ResetDocuments()
    {
        for (const auto& viewId : _documents) {
            _bridge->OnViewDestroyed(viewId);
        }
        _documents.clear();
        _warnedUnknownSources.clear();
    }

    void BridgeRuntime::Tick()
    {
        _bridge->Tick();
    }

    void BridgeRuntime::RegisterEndpoints()
    {
        _bridge->RegisterSend("close", [this](const nlohmann::json&, OSFUI::MessageBridge& a_bridge) {
            const std::string source {a_bridge.CurrentSource()};
            const auto result = _requestView ? _requestView(Runtime::ViewRequestAction::Close, source) : Runtime::ViewRequestResult::Unavailable;
            if (result != Runtime::ViewRequestResult::Accepted) {
                a_bridge.ReportProtocolFault(source, "view-unavailable", "the calling view could not be closed");
            }
        });

        const auto openView = [this](const nlohmann::json& a_payload, OSFUI::MessageBridge& a_bridge) {
            std::string viewId = OSFUI::Json::Get(a_payload, "view", "");
            if (viewId.empty()) {
                viewId = std::string {a_bridge.CurrentSource()};
            }
            RequestView(Runtime::ViewRequestAction::Open, std::move(viewId), a_bridge);
        };

        const auto closeView = [this](const nlohmann::json& a_payload, OSFUI::MessageBridge& a_bridge) {
            std::string viewId = OSFUI::Json::Get(a_payload, "view", "");
            if (viewId.empty()) {
                viewId = std::string {a_bridge.CurrentSource()};
            }
            RequestView(Runtime::ViewRequestAction::Close, std::move(viewId), a_bridge);
        };

        _bridge->RegisterRequest("menu.open", openView);
        _bridge->RegisterRequest("menu.close", closeView);
        _bridge->RegisterRequest("ping", [](const nlohmann::json&, OSFUI::MessageBridge& a_bridge) { a_bridge.Respond(nlohmann::json::object()); });
        _bridge->RegisterSend("log", [](const nlohmann::json& a_payload, OSFUI::MessageBridge&) { REX::DEBUG("MessageBridge: [web] {}", OSFUI::Json::Get(a_payload, "text", "").substr(0, 512)); });
    }

    void BridgeRuntime::HandleWebMessage(std::string_view a_viewId, std::string_view a_json)
    {
        const std::string viewId {a_viewId};
        if (!_documents.contains(viewId)) {
            constexpr std::size_t kMaxWarnedUnknownSources = 256;
            if (_warnedUnknownSources.size() < kMaxWarnedUnknownSources && _warnedUnknownSources.insert(viewId).second) {
                REX::WARN("BridgeRuntime: ignored a message from inactive view '{}'", viewId);
            }
            return;
        }

        _bridge->HandleWebMessage(viewId, a_json);
    }

    void BridgeRuntime::RequestView(Runtime::ViewRequestAction a_action, std::string a_viewId, OSFUI::MessageBridge& a_bridge)
    {
        const auto result = _requestView ? _requestView(a_action, std::move(a_viewId)) : Runtime::ViewRequestResult::Unavailable;
        switch (result) {
        case Runtime::ViewRequestResult::Accepted:
            a_bridge.Respond(nlohmann::json::object());
            return;
        case Runtime::ViewRequestResult::InvalidViewId:
            a_bridge.Reject("invalid-request", "view must be a non-empty qualified view id");
            return;
        case Runtime::ViewRequestResult::UnknownView:
        case Runtime::ViewRequestResult::NotInstantiated:
            a_bridge.Reject("unknown-view", "view is not available to this runtime");
            return;
        case Runtime::ViewRequestResult::BlockedByGameMenu:
            a_bridge.Reject("view-blocked", "views cannot open during a blocking game-menu transition");
            return;
        case Runtime::ViewRequestResult::InputUnavailable:
            a_bridge.Reject("input-unavailable", "required game-window input routing is unavailable");
            return;
        case Runtime::ViewRequestResult::QueueFull:
        case Runtime::ViewRequestResult::Unavailable:
            a_bridge.Reject("runtime-unavailable", "the view request could not be queued");
            return;
        }
    }
}
