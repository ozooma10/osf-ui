#ifdef NDEBUG
#    undef NDEBUG
#endif

#include "bridge_runtime_tests.h"

#include "v2/Bridge/BridgeRuntime.h"

#include <nlohmann/json.hpp>

#include <cassert>

namespace
{
    class RecordingTransport final : public Bridge::IViewMessageTransport
    {
    public:
        void SetWebMessageHandler(WebMessageHandler a_handler) override
        {
            handler = std::move(a_handler);
        }

        void SendMessageToWeb(std::string_view a_viewId, std::string_view a_json) noexcept override
        {
            sent.emplace_back(std::string {a_viewId}, std::string {a_json});
        }

        void Emit(std::string_view a_viewId, std::string_view a_json)
        {
            assert(handler);
            handler(a_viewId, a_json);
        }

        WebMessageHandler handler;
        std::vector<std::pair<std::string, std::string>> sent;
    };

    struct ViewRequestCall
    {
        Runtime::ViewRequestAction action;
        std::string viewId;
    };

    nlohmann::json LastEnvelope(const RecordingTransport& a_transport)
    {
        assert(!a_transport.sent.empty());
        return nlohmann::json::parse(a_transport.sent.back().second);
    }

    std::string SendEnvelope(std::string_view a_name, std::string_view a_payload = "{}")
    {
        return std::format(R"({{"kind":"send","name":"{}","payload":{}}})", a_name, a_payload);
    }

    std::string RequestEnvelope(std::string_view a_name, std::string_view a_id, std::string_view a_payload = "{}")
    {
        return std::format(R"({{"kind":"request","name":"{}","id":"{}","payload":{}}})", a_name, a_id, a_payload);
    }

    void TestBridgeHandshakeAndMinimalEndpoints()
    {
        RecordingTransport transport;
        std::vector<ViewRequestCall> requests;
        Bridge::BridgeRuntime bridge {transport, [&requests](Runtime::ViewRequestAction a_action, std::string a_viewId) {
            requests.push_back({.action = a_action, .viewId = a_viewId});
            if (a_viewId == "author.mod/missing") {
                return Runtime::ViewRequestResult::UnknownView;
            }
            return Runtime::ViewRequestResult::Accepted;
        }};

        const std::string_view viewId = "author.mod/panel";

        // Browser-host source identity is not enough by itself: the v2 runtime
        // must also have announced a live document before protocol dispatch.
        transport.Emit(viewId, SendEnvelope("osfui.hello"));
        assert(transport.sent.empty());

        bridge.OnDocumentCreated(viewId);
        transport.Emit(viewId, SendEnvelope("osfui.hello"));
        assert(transport.sent.size() == 1);
        assert(transport.sent.back().first == viewId);
        auto envelope = LastEnvelope(transport);
        assert(envelope.at("kind") == "ready");
        assert(envelope.at("payload").at("view").get<std::string>() == viewId);
        assert(envelope.at("payload").at("mod") == "author.mod");
        assert(envelope.at("payload").at("bridgeVersion") == "2.0");

        transport.Emit(viewId, RequestEnvelope("ping", "q1"));
        envelope = LastEnvelope(transport);
        assert(envelope.at("kind") == "reply");
        assert(envelope.at("id") == "q1");
        assert(envelope.at("payload").empty());

        const auto sentBeforeLog = transport.sent.size();
        transport.Emit(viewId, SendEnvelope("log", R"({"text":"hello from test"})"));
        assert(transport.sent.size() == sentBeforeLog);

        transport.Emit(viewId, RequestEnvelope("menu.open", "q2", R"({"view":"osfui/settings"})"));
        assert(requests.back().action == Runtime::ViewRequestAction::Open);
        assert(requests.back().viewId == "osfui/settings");
        envelope = LastEnvelope(transport);
        assert(envelope.at("kind") == "reply");
        assert(envelope.at("id") == "q2");

        transport.Emit(viewId, RequestEnvelope("menu.close", "q3"));
        assert(requests.back().action == Runtime::ViewRequestAction::Close);
        assert(requests.back().viewId == viewId);
        envelope = LastEnvelope(transport);
        assert(envelope.at("kind") == "reply");
        assert(envelope.at("id") == "q3");

        transport.Emit(viewId, SendEnvelope("close"));
        assert(requests.back().action == Runtime::ViewRequestAction::Close);
        assert(requests.back().viewId == viewId);

        transport.Emit(viewId, RequestEnvelope("menu.open", "q4", R"({"view":"author.mod/missing"})"));
        envelope = LastEnvelope(transport);
        assert(envelope.at("kind") == "error");
        assert(envelope.at("id") == "q4");
        assert(envelope.at("payload").at("code") == "unknown-view");

        const auto sentBeforeMalformed = transport.sent.size();
        transport.Emit(viewId, "{ malformed");
        assert(transport.sent.size() == sentBeforeMalformed);

        bridge.OnDocumentDestroyed(viewId);
        transport.Emit(viewId, RequestEnvelope("ping", "q5"));
        assert(transport.sent.size() == sentBeforeMalformed);
    }

    void TestBridgeMapsRuntimeRefusals()
    {
        RecordingTransport transport;
        Runtime::ViewRequestResult result = Runtime::ViewRequestResult::BlockedByGameMenu;
        Bridge::BridgeRuntime bridge {transport, [&result](Runtime::ViewRequestAction, std::string) { return result; }};
        bridge.OnDocumentCreated("author.mod/panel");

        transport.Emit("author.mod/panel", RequestEnvelope("menu.open", "q1"));
        auto envelope = LastEnvelope(transport);
        assert(envelope.at("payload").at("code") == "view-blocked");

        result = Runtime::ViewRequestResult::QueueFull;
        transport.Emit("author.mod/panel", RequestEnvelope("menu.open", "q2"));
        envelope = LastEnvelope(transport);
        assert(envelope.at("payload").at("code") == "runtime-unavailable");

        result = Runtime::ViewRequestResult::InputUnavailable;
        transport.Emit("author.mod/panel", RequestEnvelope("menu.open", "q3"));
        envelope = LastEnvelope(transport);
        assert(envelope.at("payload").at("code") == "input-unavailable");

        bridge.ResetDocuments();
        const auto sentBeforeInactive = transport.sent.size();
        transport.Emit("author.mod/panel", RequestEnvelope("ping", "q4"));
        assert(transport.sent.size() == sentBeforeInactive);
    }
}

void RunBridgeRuntimeTests()
{
    TestBridgeHandshakeAndMinimalEndpoints();
    TestBridgeMapsRuntimeRefusals();
}
