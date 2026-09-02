#include <SFSE/SFSE.h>
#include <nlohmann/json.hpp>

#include <algorithm>
#include <string>

#include "OSFUI_Views.h"

namespace
{
    using Json = nlohmann::json;
    OSFUI::API::Views::Client g_views;

    constexpr const char* kModId = "__OSFUI_MOD_ID__";
    constexpr const char* kViewId = "__OSFUI_MOD_ID__/__OSFUI_VIEW_ID__";
    constexpr const char* kNotice = "__OSFUI_MOD_ID__.notice";
    int g_count = 0;

    template <class T>
    std::string Encode(const T& value) noexcept
    {
        try { return Json(value).dump(-1, ' ', false, Json::error_handler_t::replace); }
        catch (...) { return "null"; }
    }

    void PushState() noexcept
    {
        const auto json = Encode(Json{
            { "count", g_count },
            { "lastAction", "State explicitly forwarded by the native owner" }
        });
        (void)g_views.SetViewState(kModId, "state", json.c_str());
    }

    void OnIncrement(const char*, const char* payloadJson, const char* sourceViewId, void*) noexcept
    {
        auto payload = Json::parse(payloadJson ? payloadJson : "{}", nullptr, false);
        if (!payload.is_object()) return;
        const int amount = payload.value("amount", 1);
        g_count += std::clamp(amount, -10, 10);
        PushState();
        const auto event = Encode(Json{ { "message", "Native owner handled the send" } });
        (void)g_views.SendToWeb(sourceViewId && *sourceViewId ? sourceViewId : kViewId,
            kNotice, event.c_str());
    }

    void OnGetState(const OSFUI::API::Views::Request& request, void*) noexcept
    {
        const auto response = Encode(Json{ { "count", g_count } });
        request.Respond(response.c_str());
    }

    void OnGreet(const OSFUI::API::Views::Request& request, void*) noexcept
    {
        auto payload = Json::parse(request.payloadJson ? request.payloadJson : "{}", nullptr, false);
        if (!payload.is_object() || !payload.contains("name") || !payload["name"].is_string()) {
            request.Reject("invalid-payload", "name must be a string");
            return;
        }
        const auto response = Encode(Json{
            { "message", "Hello, " + payload["name"].get<std::string>() + "!" }
        });
        request.Respond(response.c_str());
    }

    void OnBridgeAvailable(void*) noexcept { PushState(); }

    void OnSFSEMessage(SFSE::MessagingInterface::Message* message)
    {
        if (message->type != SFSE::MessagingInterface::kPostLoad || !g_views.Init()) return;
        g_views.RegisterSend("__OSFUI_MOD_ID__.increment", &OnIncrement, nullptr);
        g_views.RegisterRequest("__OSFUI_MOD_ID__.getState", &OnGetState, nullptr);
        g_views.RegisterRequest("__OSFUI_MOD_ID__.greet", &OnGreet, nullptr);
        g_views.SetReadyCallback(&OnBridgeAvailable, nullptr);
        (void)g_views.RegisterView(kViewId);
    }
}

SFSE_PLUGIN_LOAD(const SFSE::LoadInterface* sfse)
{
    SFSE::Init(sfse);
    const auto messaging = SFSE::GetMessagingInterface();
    if (!messaging) return false;
    messaging->RegisterListener(OnSFSEMessage);
    return true;
}
