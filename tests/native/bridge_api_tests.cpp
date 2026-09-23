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
    auto presentation = api.TakeViewPresentationRequests();
    CHECK(presentation.size() == 1 && presentation[0].view == "acme/panel" && presentation[0].open);

    const auto interactionCallback = +[](API::InteractionToken, const char*, API::InteractionPhase, const char*, void*) noexcept {};
    CHECK(api.BeginInteraction("missing/screen", interactionCallback, nullptr) == 0);
    CHECK(api.BeginInteraction("acme/panel", nullptr, nullptr) == 0);
    CHECK(!api.EndInteraction(0));
    const auto token = api.BeginInteraction("ACME/panel", interactionCallback, &g_ready);
    CHECK(token != 0);
    CHECK(api.EndInteraction(token));
    const auto interactions = api.TakePendingBatch().interactions;
    CHECK(interactions.size() == 2);
    CHECK(interactions[0].view == "acme/panel" && interactions[0].token == token);
    CHECK(interactions[0].context == &g_ready && interactions[0].callback == interactionCallback);
    CHECK(interactions[1].view.empty() && interactions[1].token == token && !interactions[1].callback);
    CHECK(api.TakePendingBatch().interactions.empty());
    for (int i = 0; i < 64; ++i) CHECK(api.BeginInteraction("acme/panel", interactionCallback, nullptr) > token);
    CHECK(api.BeginInteraction("acme/panel", interactionCallback, nullptr) == 0);
    CHECK(!api.EndInteraction(token));
    CHECK(api.TakePendingBatch().interactions.size() == 64);

    API::Client client;
    CHECK(client.Attach(&api));
    const auto clientToken = client.BeginInteraction("acme/panel", interactionCallback, nullptr);
    CHECK(clientToken > token);
    CHECK(client.EndInteraction(clientToken));
    CHECK(api.TakePendingBatch().interactions.size() == 2);
    CHECK(!client.Attach(&api, API::kVersion + 1));
    CHECK(!client && client.Version() == 0);
    CHECK(client.BeginInteraction("acme/panel", interactionCallback, nullptr) == 0);
    CHECK(!client.EndInteraction(token));

    CHECK(api.RegisterView("acme/panel"));
    CHECK(!api.RegisterView("osfui/settings"));
    auto registrations = api.TakeViewRegistrations();
    CHECK(registrations == std::vector<std::string>{ "acme/panel" });

    CHECK(api.SetViewState("acme", "status", R"({"ready":true})"));
    CHECK(!api.SetViewState("acme", "status", "{bad"));
    auto state = api.TakeViewStateOps();
    CHECK(state.size() == 1 && state[0].mod == "acme" && state[0].key == "status");

    CHECK(api.RegisterRelativePointer("acme/panel", &Pointer, nullptr));
    CHECK(!api.RegisterRelativePointer("acme/panel", &Pointer, nullptr));
    CHECK(api.DispatchRelativePointer("acme/panel", API::RelativePointerPhase::kBegin));
    CHECK(g_pointer == 1);
    api.UnregisterRelativePointer("acme/panel");

    MessageBridge bridge(Capture);
    api.RegisterSend("acme.increment", &Send, nullptr);
    api.SetBridgeAvailability(&bridge);
    api.PumpMainThread();
    CHECK(api.IsReady());

    // A subscriber installed after acquisition is still replayed on the game thread.
    api.SetReadyCallback(&Ready, nullptr);
    api.PumpMainThread();
    CHECK(g_ready == 1);

    bridge.OnViewCreated("acme/panel");
    bridge.HandleWebMessage("acme/panel",
        R"({"kind":"send","name":"osfui.hello","payload":{}})");
    bridge.HandleWebMessage("acme/panel",
        R"({"kind":"send","name":"acme.increment","payload":{"amount":1}})");
    CHECK(g_send == 1);

    // A failed world host stops draining native sends. Reattaching a replacement
    // restores availability, but delivery still waits for its document greeting.
    api.SetViewInstantiated("acme/panel", false);
    bridge.OnViewDestroyed("acme/panel");
    api.SetBridgeAvailability(nullptr);
    api.PumpMainThread();
    g_sent.clear();
    CHECK(api.SendToWeb("acme/panel", "acme.during-recovery", R"({"value":7})"));
    api.PumpMainThread();
    CHECK(g_sent.empty());
    CHECK(!api.IsReady());

    bridge.OnViewCreated("acme/panel");
    api.SetBridgeAvailability(&bridge);
    api.SetViewInstantiated("acme/panel", true);
    api.PumpMainThread();
    CHECK(api.IsReady());
    CHECK(g_sent.empty());
    bridge.HandleWebMessage("acme/panel",
        R"({"kind":"send","name":"osfui.hello","payload":{}})");
    CHECK(g_sent.size() == 2);
    CHECK(g_sent[0]["kind"] == "ready");
    CHECK(g_sent[1]["kind"] == "event");
    CHECK(g_sent[1]["name"] == "acme.during-recovery");
    CHECK(g_sent[1]["payload"]["value"] == 7);

    api.UnregisterSend("acme.increment");
    api.SetReadyCallback(nullptr, nullptr);
    api.SetBridgeAvailability(nullptr);
    api.PumpMainThread();
    CHECK(!api.IsReady());

    std::fprintf(stderr, "bridge_api_tests: %d checks, %d failures\n", g_checks, g_failures);
    return g_failures;
}
