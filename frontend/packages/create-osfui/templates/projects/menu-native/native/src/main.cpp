#include <SFSE/SFSE.h>
#include <algorithm>
#include <cstring>
#include <string>
#include "OSFUI_JSON.h"
#include "OSFUI_Settings.h"
#include "OSFUI_Views.h"

namespace
{
    OSFUI::API::Settings::Client g_settings;
    OSFUI::API::Views::Client g_views;
    OSFUI::API::JsonClient g_json{ g_views };

    constexpr const char* kModId = "__OSFUI_MOD_ID__";
    constexpr const char* kViewId = "__OSFUI_MOD_ID__/__OSFUI_VIEW_ID__";
    constexpr const char* kNoticeType = "__OSFUI_MOD_ID__.notice";

    struct DemoState
    {
        int count{ 0 };
        bool enabled{ true };
        std::string greeting{ "Hello from the __OSFUI_PLUGIN_NAME__ native plugin" };
        const char* lastAction{ "Plugin initialized" };
    };

    void to_json(OSFUI::API::Json& json, const DemoState& state)
    {
        json = {
            { "count", state.count },
            { "enabled", state.enabled },
            { "greeting", state.greeting },
            { "lastAction", state.lastAction },
            { "features", OSFUI::API::Json::array({
                "typed JSON", "sends", "requests", "native pushes", "settings", "hotkeys"
            }) }
        };
    }

    DemoState g_state;

    // Retained state; replayed after reload.
    void PushState() noexcept
    {
        g_json.SetViewState(kModId, "state", g_state);
    }

    // One-shot event.
    void PushNotice(const char* viewId, const char* message) noexcept
    {
        g_json.SendToWeb(viewId, kNoticeType, OSFUI::API::Json{ { "message", message } });
    }

    // One-way send.
    void OnIncrement(const char* name, const char* payloadJson, const char* sourceViewId, void*) noexcept
    {
        OSFUI::API::JsonSend event{ name, payloadJson, sourceViewId };
        const char* target = event.SourceViewId().empty() ? kViewId : event.SourceViewId().data();
        if (!event) {
            PushNotice(target, "C++ ignored a malformed send payload");
            return;
        }
        if (!g_state.enabled) {
            PushNotice(target, "The native counter is disabled in Mod Settings");
            return;
        }

        g_state.count += std::clamp(event.Value("amount", 1), -10, 10);
        g_state.lastAction = "JavaScript sent a fire-and-forget message";
        PushState();
    }

    // Request/reply.
    void OnGetState(const OSFUI::API::Views::Request& raw, void*) noexcept
    {
        OSFUI::API::JsonRequest request{ raw };
        if (request) {
            request.Respond(g_state);
        }
    }

    // Typed request.
    void OnGreet(const OSFUI::API::Views::Request& raw, void*) noexcept
    {
        OSFUI::API::JsonRequest request{ raw };
        if (!request) return;
        const auto name = request.Get<std::string>("name");
        if (!name) return;
        if (name->empty()) {
            request.Reject("invalid-payload", "name must not be empty");
            return;
        }
        const bool excited = request.Value("excited", false);

        try {
            request.Respond("__OSFUI_MOD_ID__.greeting", OSFUI::API::Json{
                { "message", g_state.greeting + ", " + *name + (excited ? "!!" : "!") },
                { "receivedFromJs", request.Payload() },
                { "nativeCount", g_state.count }
            });
        } catch (...) {
            request.Reject("example-error", "could not build the greeting");
        }
    }

    // Settings action request.
    void OnRecalibrate(const OSFUI::API::Views::Request& raw, void*) noexcept
    {
        OSFUI::API::JsonRequest request{ raw };
        if (!request) return;
        g_state.count = 0;
        g_state.lastAction = "Native settings action completed";
        PushState();
        request.Respond(OSFUI::API::Json{ { "message", "Example recalibration complete" } });
    }

    // Bridge ready or recreated.
    void OnBridgeAvailable(void*) noexcept
    {
        g_state.lastAction = "Bridge-availability callback fired";
        PushState();
    }

    // Initial replay and later changes.
    void OnSetting(const char*, const char* key, const char* valueJson, void*) noexcept
    {
        try {
            const auto value = OSFUI::API::Json::parse(valueJson ? valueJson : "null", nullptr, false);
            if (value.is_discarded()) return;
            if (std::strcmp(key, "enabled") == 0 && value.is_boolean()) {
                g_state.enabled = value.get<bool>();
            } else if (std::strcmp(key, "greeting") == 0 && value.is_string()) {
                g_state.greeting = value.get<std::string>();
            } else {
                return;
            }
            g_state.lastAction = "C++ settings callback applied a value";
            PushState();
        } catch (...) {}
    }

    void OnHotkey(const char*, const char*, void*) noexcept
    {
        g_state.lastAction = "C++ hotkey callback opened the view";
        PushState();
        PushNotice(kViewId, "The native open-view hotkey fired");
        (void)g_views.RequestMenu(kViewId, true);
    }

    void OnSFSEMessage(SFSE::MessagingInterface::Message* message)
    {
        if (message->type != SFSE::MessagingInterface::kPostLoad) return;
        if (!g_views.Init()) return;  // Optional dependency.
        (void)g_settings.Init();

        g_views.RegisterSend("__OSFUI_MOD_ID__.increment", &OnIncrement, nullptr);
        g_views.RegisterRequest("__OSFUI_MOD_ID__.getState", &OnGetState, nullptr);
        g_views.RegisterRequest("__OSFUI_MOD_ID__.greet", &OnGreet, nullptr);
        g_views.RegisterRequest("__OSFUI_MOD_ID__.recalibrate", &OnRecalibrate, nullptr);
        g_views.SetReadyCallback(&OnBridgeAvailable, nullptr);
        (void)g_views.RegisterView(kViewId);
        (void)g_settings.SubscribeSettings(kModId, &OnSetting, nullptr);
        (void)g_settings.SubscribeHotkey(kModId, "openKey", &OnHotkey, nullptr);
    }

}
SFSE_PLUGIN_LOAD(const SFSE::LoadInterface* sfse)
{
    SFSE::Init(sfse);
    auto* messaging = SFSE::GetMessagingInterface();
    if (!messaging) return false;
    messaging->RegisterListener(OnSFSEMessage);
    return true;
}
