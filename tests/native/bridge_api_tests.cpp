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
    void Pointer(const char*, OSFUI::API::Views::RelativePointerPhase, float, float, float, void*) noexcept
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
    CHECK(api.DispatchRelativePointer("acme/panel", API::Views::RelativePointerPhase::kBegin));
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

    api.UnregisterSend("acme.increment");
    api.SetReadyCallback(nullptr, nullptr);
    api.SetBridgeAvailability(nullptr);
    api.PumpMainThread();
    CHECK(!api.IsReady());

    std::fprintf(stderr, "bridge_api_tests: %d checks, %d failures\n", g_checks, g_failures);
    return g_failures;
}
