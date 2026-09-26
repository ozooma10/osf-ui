#include "API/BridgeApi.h"
#include "Bridge/MessageBridge.h"
#include "Core/Log.h"
#include "check.h"

namespace
{
    std::vector<nlohmann::json> g_sent;
    int g_ready = 0;
    int g_pointer = 0;
    int g_send = 0;

    void Capture(std::string_view, std::string_view json)
    {
        g_sent.push_back(nlohmann::json::parse(json));
    }

    void Ready(void*) noexcept { ++g_ready; }
    void Pointer(const char*, OSFUI::API::RelativePointerPhase, float, float, float, void*) noexcept
    {
        ++g_pointer;
    }
    void Send(const char*, const char*, const char*, void*) noexcept { ++g_send; }
}

namespace OSFUI::Log
{
    void WarnOnce(std::once_flag& flag, std::string_view message)
    {
        std::call_once(flag, [&] { REX::test::Log("WARN", std::string(message)); });
    }
    bool DebugEnabled() { return true; }
    void SetDebugLogging(bool) {}
}

int main()
{
    using namespace OSFUI;
    auto& api = API::BridgeApi::Get();

    CHECK(API::IsUnreservedEndpointName("acme.increment"));
    CHECK(!API::IsUnreservedEndpointName("osfui.anything"));
    CHECK(!API::IsUnreservedEndpointName("close"));

    api.SetViewCatalog({ "acme/panel", "acme/hud" });
    CHECK(api.RequestMenu("acme/panel", true));
    CHECK(!api.RequestMenu("missing/panel", true));
    auto presentation = api.TakePendingBatch().presentation;
    CHECK(presentation.size() == 1);
    const auto& opened = std::get<ViewRequestQueue::ViewRequest>(presentation.at(0));
    CHECK(opened.view == "acme/panel" && opened.open);

    // Native, browser and input producers share one FIFO, including closes of
    // views that an earlier queued open has not instantiated yet.
    api.ViewRequests().EnqueueView("acme/hud", true);
    CHECK(api.RequestMenu("ACME/PANEL", true));
    api.ViewRequests().Enqueue(ViewPresentationRequest::Back);
    CHECK(api.RequestMenu("acme/panel", false));
    CHECK(!api.RequestMenu("missing/panel", false));
    auto mixed = api.TakePendingBatch().presentation;
    CHECK(mixed.size() == 4);
    CHECK(std::get<ViewRequestQueue::ViewRequest>(mixed.at(0)).view == "acme/hud");
    CHECK(std::get<ViewRequestQueue::ViewRequest>(mixed.at(1)).view == "acme/panel");
    CHECK(std::get<ViewPresentationRequest>(mixed.at(2)) == ViewPresentationRequest::Back);
    CHECK(!std::get<ViewRequestQueue::ViewRequest>(mixed.at(3)).open);
    CHECK(api.TakePendingBatch().presentation.empty());

    API::Client client;
    CHECK(client.Attach(&api));
    CHECK(client.Attach(&api, API::kVersion + 1)); // Minor bumps stay compatible.
    CHECK(!API::Supports(API::kVersion, API::kVersion + 0x00010000u)); // Runtime rejects incompatible majors.
    CHECK(!client.Attach(&api, API::kVersion + 0x00010000u)); // Major bumps detach.
    CHECK(!client && client.Version() == 0);

    CHECK(api.RegisterView("acme/panel"));
    CHECK(!api.RegisterView("osfui/settings"));
    auto registrations = api.TakePendingBatch().viewRegistrations;
    CHECK(registrations == std::vector<std::string>{ "acme/panel" });

    CHECK(api.SetViewState("acme", "status", R"({"ready":true})"));
    CHECK(!api.SetViewState("acme", "status", "{bad"));
    auto state = api.TakePendingBatch().state;
    CHECK(state.size() == 1 && state[0].mod == "acme" && state[0].key == "status");

    // Draining state preserves publication order and leaves requests and
    // registrations queued for the next batch.
    CHECK(api.SetViewState("acme", "status", R"({"revision":1})"));
    CHECK(api.SetViewState("acme", "status", R"({"revision":2})"));
    CHECK(api.RequestMenu("acme/hud", true));
    api.ViewRequests().Enqueue(ViewPresentationRequest::Back);
    CHECK(api.RegisterView("acme/hud"));
    auto pendingState = api.TakePendingState();
    CHECK(pendingState.size() == 2);
    if (pendingState.size() == 2) {
        CHECK(pendingState[0].value["revision"] == 1);
        CHECK(pendingState[1].value["revision"] == 2); // Newest publication wins.
    }
    CHECK(api.TakePendingState().empty());
    auto afterState = api.TakePendingBatch();
    CHECK(afterState.state.empty());
    CHECK(afterState.presentation.size() == 2);
    CHECK(std::get<ViewRequestQueue::ViewRequest>(afterState.presentation.at(0)).view == "acme/hud");
    CHECK(std::get<ViewPresentationRequest>(afterState.presentation.at(1)) == ViewPresentationRequest::Back);
    CHECK(afterState.viewRegistrations == std::vector<std::string>{ "acme/hud" });
    // An empty state drain must not swallow a subsequent publication.
    CHECK(api.SetViewState("acme", "status", R"({"revision":3})"));
    auto laterState = api.TakePendingBatch().state;
    CHECK(laterState.size() == 1 && laterState[0].value["revision"] == 3);

    CHECK(api.RegisterRelativePointer("acme/panel", &Pointer, nullptr));
    CHECK(!api.RegisterRelativePointer("acme/panel", &Pointer, nullptr));
    CHECK(api.DispatchRelativePointer("acme/panel", API::RelativePointerPhase::kBegin));
    CHECK(g_pointer == 1);
    api.UnregisterRelativePointer("acme/panel");

    MessageBridge bridge(Capture);
    CHECK(api.RegisterSend("acme.increment", &Send, nullptr));
    CHECK(!api.RegisterSend("acme.increment", &Send, nullptr)); // Duplicate name refused.
    CHECK(!api.RegisterSend("osfui.reserved", &Send, nullptr));
    api.SetBridgeAvailability(&bridge);
    api.PumpRuntimeCallbacks();
    CHECK(api.IsReady());

    // A subscriber installed after acquisition is still replayed on the game thread.
    api.SetReadyCallback(&Ready, nullptr);
    api.PumpRuntimeCallbacks();
    CHECK(g_ready == 1);

    bridge.OnViewCreated("acme/panel");
    bridge.HandleWebMessage("acme/panel",
        R"({"kind":"send","name":"osfui.hello","payload":{}})");
    bridge.HandleWebMessage("acme/panel",
        R"({"kind":"send","name":"acme.increment","payload":{"amount":1}})");
    CHECK(g_send == 1);

    // A failed browser host stops draining native sends. Reattaching a replacement
    // restores availability, but delivery still waits for its document greeting.
    api.SetViewInstantiated("acme/panel", false);
    bridge.OnViewDestroyed("acme/panel");
    api.SetBridgeAvailability(nullptr);
    api.PumpRuntimeCallbacks();
    g_sent.clear();
    CHECK(api.SendToWeb("acme/panel", "acme.during-recovery", R"({"value":7})"));
    api.PumpRuntimeCallbacks();
    CHECK(g_sent.empty());
    CHECK(!api.IsReady());

    bridge.OnViewCreated("acme/panel");
    api.SetBridgeAvailability(&bridge);
    api.SetViewInstantiated("acme/panel", true);
    api.PumpRuntimeCallbacks();
    CHECK(api.IsReady());
    CHECK(g_sent.empty());
    bridge.HandleWebMessage("acme/panel",
        R"({"kind":"send","name":"osfui.hello","payload":{}})");
    CHECK(g_sent.size() == 2);
    if (g_sent.size() == 2) {
        CHECK(g_sent[0]["kind"] == "ready");
        CHECK(g_sent[1]["kind"] == "event");
        CHECK(g_sent[1]["name"] == "acme.during-recovery");
        CHECK(g_sent[1]["payload"]["value"] == 7);
    }


    CHECK(g_ready == 2); // readiness fires again after recreation
    CHECK(api.RegisterRelativePointer("ACME/PANEL", &Pointer, nullptr));
    CHECK(!api.RegisterRelativePointer("Acme/panel", &Pointer, nullptr));
    CHECK(api.DispatchRelativePointer("acme/panel", API::RelativePointerPhase::kBegin));
    api.UnregisterRelativePointer("AcMe/PaNeL");
    CHECK(!api.HasRelativePointer("acme/panel"));
    g_sent.clear();
    CHECK(api.SendToWeb("ACME/PANEL", "mixed-case", "{}"));
    api.PumpRuntimeCallbacks();
    CHECK(g_sent.size() == 1 && g_sent.back()["name"] == "mixed-case");
    g_sent.clear();
    CHECK(api.SendToWeb("acme/panel", "commented-json", R"({/* allowed input */"value":3})"));
    api.PumpRuntimeCallbacks();
    CHECK(g_sent.size() == 1 && g_sent.back()["payload"]["value"] == 3);
    g_sent.clear();
    const std::string qualifiedEvent = std::string(64, 'a') + "." + std::string(125, 'b');
    bridge.Emit("acme/panel", qualifiedEvent, nlohmann::json::object());
    CHECK(g_sent.size() == 1 && g_sent.back()["name"] == qualifiedEvent);
    CHECK(api.ClaimPapyrusEndpoint("acme.refresh"));
    CHECK(!api.RegisterSend("Acme.Refresh", &Send, nullptr));
    CHECK(!api.ClaimPapyrusEndpoint("ACME.INCREMENT"));
    api.ReleasePapyrusEndpoint("acme.refresh");
    CHECK(api.RegisterSend("acme.refresh", &Send, nullptr));

    // A late reply to an old q1 must not settle the new document's q1.
    MessageBridge::DeferToken oldToken = 0, newToken = 0;
    bridge.RegisterRequest("deferred", [&](const auto&, MessageBridge& source) {
        const auto token = source.Defer();
        if (oldToken == 0) oldToken = token;
        else newToken = token;
    });
    bridge.HandleWebMessage("acme/panel", R"({"kind":"request","name":"deferred","id":"q1","payload":{}})");
    bridge.OnViewCreated("acme/panel");
    bridge.HandleWebMessage("acme/panel", R"({"kind":"request","name":"deferred","id":"q1","payload":{}})");
    CHECK(oldToken != 0 && newToken != 0 && oldToken != newToken);
    g_sent.clear();
    bridge.RespondTo(oldToken, {{ "old", true }});
    CHECK(g_sent.empty());
    bridge.RespondTo(newToken, {{ "new", true }});
    CHECK(g_sent.size() == 1 && g_sent.back()["payload"]["new"] == true);
    // Only the first answer counts; the bridge drops the duplicate.
    bridge.RejectTo(newToken, "late");
    CHECK(g_sent.size() == 1);

    // RejectAll settles every outstanding request once with the given error.
    oldToken = newToken = 0;
    bridge.HandleWebMessage("acme/panel", R"({"kind":"request","name":"deferred","id":"q2","payload":{}})");
    bridge.HandleWebMessage("acme/panel", R"({"kind":"request","name":"deferred","id":"q3","payload":{}})");
    g_sent.clear();
    bridge.RejectAll("game-load", "canceled");
    CHECK(g_sent.size() == 2);
    for (const auto& sent : g_sent) {
        CHECK(sent["kind"] == "error");
        CHECK(sent["payload"]["code"] == "game-load");
    }
    g_sent.clear();
    bridge.RespondTo(oldToken, {{ "late", true }});
    bridge.RespondTo(newToken, {{ "late", true }});
    CHECK(g_sent.empty());

    // Tokens are process-unique, so a stale answer cannot settle a request on a new bridge.
    MessageBridge other([](std::string_view, std::string_view) {});
    MessageBridge::DeferToken otherToken = 0;
    other.RegisterRequest("deferred", [&](const auto&, MessageBridge& source) { otherToken = source.Defer(); });
    other.OnViewCreated("acme/panel");
    other.HandleWebMessage("acme/panel", R"({"kind":"request","name":"deferred","id":"q1","payload":{}})");
    CHECK(otherToken > newToken);

    // Owner-qualified fallback beats a global native endpoint, including kind checks.
    unsigned ownSend = 0, globalSend = 0;
    bridge.RegisterSend("refresh", [&](const auto&, auto&) { ++globalSend; });
    bridge.SetEndpointFallback(
        [](std::string_view, std::string_view name) {
            if (name == "acme.refresh") return MessageBridge::FallbackEndpointKind::kSend;
            if (name == "acme.fetch") return MessageBridge::FallbackEndpointKind::kRequest;
            return MessageBridge::FallbackEndpointKind::kNone;
        },
        [&](std::string_view name, const auto&, auto&) { CHECK(name == "acme.refresh"); ++ownSend; },
        [](std::string_view name, const auto&, MessageBridge& source) { CHECK(name == "acme.fetch"); source.Respond({}); });
    // This isolated bridge has no native acme.refresh: BridgeApi has not pumped it yet.
    bridge.HandleWebMessage("acme/panel", R"({"kind":"send","name":"refresh","payload":{}})");
    CHECK(ownSend == 1 && globalSend == 0);
    bridge.RegisterSend("fetch", [&](const auto&, auto&) { ++globalSend; });
    bridge.HandleWebMessage("acme/panel", R"({"kind":"send","name":"fetch","payload":{}})");
    CHECK(globalSend == 0);
    bridge.HandleWebMessage("acme/panel", R"({"kind":"request","name":"fetch","id":"q2","payload":{}})");
    CHECK(g_sent.back()["kind"] == "reply" && g_sent.back()["id"] == "q2");

    api.SetViewInstantiated("acme/panel", true); // dev reload with the same bridge/view ID
    api.PumpRuntimeCallbacks();
    CHECK(g_ready == 3);
    api.PumpRuntimeCallbacks();
    CHECK(g_ready == 3);
    api.SetReadyCallback(nullptr, nullptr);
    api.SetBridgeAvailability(nullptr);
    api.PumpRuntimeCallbacks();
    CHECK(!api.IsReady());

    std::fprintf(stderr, "bridge_api_tests: %d checks, %d failures\n", g_checks, g_failures);
    return g_failures;
}
