#include <SFSE/SFSE.h>
#include <nlohmann/json.hpp>
#include <algorithm>
#include <cstring>
#include <optional>
#include <string>
#include "OSFUI_Settings.h"
#include "OSFUI_Views.h"

namespace
{
    using Json = nlohmann::json;

    OSFUI::API::Settings::Client g_settings;
    OSFUI::API::Views::Client g_views;

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

    void to_json(Json& json, const DemoState& state)
    {
        json = {
            { "count", state.count },
            { "enabled", state.enabled },
            { "greeting", state.greeting },
            { "lastAction", state.lastAction },
            { "features", Json::array({
                "typed JSON", "sends", "requests", "native pushes", "settings", "hotkeys"
            }) }
        };
    }

    DemoState g_state;

    std::optional<Json> ParseObject(const char* text) noexcept
    {
        try {
            auto value = Json::parse(text ? text : "{}", nullptr, false);
            if (value.is_object()) return value;
        } catch (...) {}
        return std::nullopt;
    }

    std::optional<Json> ParseRequest(const OSFUI::API::Views::Request& request) noexcept
    {
        auto payload = ParseObject(request.payloadJson);
        if (!payload) request.Reject("invalid-payload", "payload must be a JSON object");
        return payload;
    }

    template <class T>
    std::optional<T> Required(const OSFUI::API::Views::Request& request,
        const Json& payload, const char* key) noexcept
    {
        try {
            const auto found = payload.find(key);
            if (found != payload.end()) return found->get<T>();
        } catch (...) {}
        request.Reject("invalid-payload", "missing or invalid required field");
        return std::nullopt;
    }

    template <class T>
    T Value(const Json& payload, const char* key, T fallback) noexcept
    {
        try {
            const auto found = payload.find(key);
            if (found != payload.end()) return found->get<T>();
        } catch (...) {}
        return fallback;
    }

    template <class T>
    bool SendEvent(const char* viewId, const char* type, const T& value) noexcept
    {
        try {
            const auto text = Json(value).dump(-1, ' ', false, Json::error_handler_t::replace);
            return g_views.SendToWeb(viewId, type, text.c_str());
        } catch (...) {
            return false;
        }
    }

    template <class T>
    bool SetState(const char* modId, const char* key, const T& value) noexcept
    {
        try {
            const auto text = Json(value).dump(-1, ' ', false, Json::error_handler_t::replace);
            return g_views.SetViewState(modId, key, text.c_str());
        } catch (...) {
            return false;
        }
    }

    template <class T>
    bool Reply(const OSFUI::API::Views::Request& request, const T& value,
        const char* type = nullptr) noexcept
    {
        try {
            const auto text = Json(value).dump(-1, ' ', false, Json::error_handler_t::replace);
            if (type && *type) request.Respond(type, text.c_str());
            else request.Respond(text.c_str());
            return true;
        } catch (...) {
            request.Reject("serialization-error", "could not serialize response");
            return false;
        }
    }

    // Retained state; replayed after reload.
    void PushState() noexcept
    {
        (void)SetState(kModId, "state", g_state);
    }

    // One-shot event.
    void PushNotice(const char* viewId, const char* message) noexcept
    {
        try {
            (void)SendEvent(viewId, kNoticeType, Json{ { "message", message } });
        } catch (...) {}
    }

    // One-way send.
    void OnIncrement(const char* name, const char* payloadJson, const char* sourceViewId, void*) noexcept
    {
        (void)name;
        const char* target = sourceViewId && *sourceViewId ? sourceViewId : kViewId;
        const auto payload = ParseObject(payloadJson);
        if (!payload) {
            PushNotice(target, "C++ ignored a malformed send payload");
            return;
        }
        if (!g_state.enabled) {
            PushNotice(target, "The native counter is disabled in Mod Settings");
            return;
        }

        g_state.count += std::clamp(Value(*payload, "amount", 1), -10, 10);
        g_state.lastAction = "JavaScript sent a fire-and-forget message";
        PushState();
    }

    // Request/reply.
    void OnGetState(const OSFUI::API::Views::Request& raw, void*) noexcept
    {
        if (!ParseRequest(raw)) return;
        (void)Reply(raw, g_state);
    }

    // Typed request.
    void OnGreet(const OSFUI::API::Views::Request& raw, void*) noexcept
    {
        const auto payload = ParseRequest(raw);
        if (!payload) return;
        const auto name = Required<std::string>(raw, *payload, "name");
        if (!name) return;
        if (name->empty()) {
            raw.Reject("invalid-payload", "name must not be empty");
            return;
        }
        const bool excited = Value(*payload, "excited", false);

        try {
            (void)Reply(raw, Json{
                { "message", g_state.greeting + ", " + *name + (excited ? "!!" : "!") },
                { "receivedFromJs", *payload },
                { "nativeCount", g_state.count }
            }, "__OSFUI_MOD_ID__.greeting");
        } catch (...) {
            raw.Reject("example-error", "could not build the greeting");
        }
    }

    // Settings action request.
    void OnRecalibrate(const OSFUI::API::Views::Request& raw, void*) noexcept
    {
        if (!ParseRequest(raw)) return;
        g_state.count = 0;
        g_state.lastAction = "Native settings action completed";
        PushState();
        try {
            (void)Reply(raw, Json{ { "message", "Example recalibration complete" } });
        } catch (...) {
            raw.Reject("serialization-error", "could not serialize response");
        }
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
            const auto value = Json::parse(valueJson ? valueJson : "null", nullptr, false);
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
