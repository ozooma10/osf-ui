#include <SFSE/SFSE.h>
#include <algorithm>
#include <cstring>
#include <string>
#include "OSFUI_JSON.h"

namespace
{
    OSFUI::API::Client g_ui;
    OSFUI::API::JsonClient g_json{ g_ui };

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

    // STATE, not a push: SetViewState retains the value and OSF UI replays it
    // to every document of this mod — first open, F5, dev reload, crash
    // recovery. Its owning view subscribes once with osfui.state.on("state") and is
    // never blank, and there is no "the view reloaded, re-send me everything"
    // handshake on either side. The viewId parameter is gone: state is
    // addressed by MOD, so every view of the mod gets it.
    void PushState() noexcept
    {
        (void)g_json.SetViewState(kModId, "state", g_state);
    }

    // A notice is something that HAPPENED, so it is an event: delivered once,
    // never replayed. Compare PushState below, which publishes STATE — the
    // OSF UI runtime replays that to every document, including after an F5, so the
    // view never has to ask for it.
    void PushNotice(const char* viewId, const char* message) noexcept
    {
        try {
            (void)g_json.SendToWeb(viewId, kNoticeType,
                OSFUI::API::Json{ { "message", message } });
        } catch (...) {}
    }

    // A registered SEND endpoint is one-way, with nothing to settle.
    // Owning-view JavaScript: osfui.send("increment", { amount: 1 })
    void OnIncrement(const char* name, const char* payloadJson,
        const char* sourceViewId, void*) noexcept
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

    // A registered REQUEST settles exactly once, with a payload or a code.
    // Owning-view JavaScript: const state = await osfui.request("getState")
    void OnGetState(const OSFUI::API::Request& raw, void*) noexcept
    {
        OSFUI::API::JsonRequest request{ raw };
        if (request) (void)request.Respond(g_state);
    }

    // JsonRequest validates required fields and owns request correlation.
    void OnGreet(const OSFUI::API::Request& raw, void*) noexcept
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
            (void)request.Respond("__OSFUI_MOD_ID__.greeting", OSFUI::API::Json{
                { "message", g_state.greeting + ", " + *name + (excited ? "!!" : "!") },
                { "receivedFromJs", request.Payload() },
                { "nativeCount", g_state.count }
            });
        } catch (...) {
            request.Reject("example-error", "could not build the greeting");
        }
    }

    // Settings action rows are requests too. The built-in Mod Settings view
    // shows this reply message as a toast.
    void OnRecalibrate(const OSFUI::API::Request& raw, void*) noexcept
    {
        OSFUI::API::JsonRequest request{ raw };
        if (!request) return;
        g_state.count = 0;
        g_state.lastAction = "Native settings action completed";
        PushState();
        (void)request.Respond(OSFUI::API::Json{ { "message", "Example recalibration complete" } });
    }

    // Main-thread callback fired when the bridge becomes available (and after recreation).
    void OnBridgeAvailable(void*) noexcept
    {
        g_state.lastAction = "Bridge-availability callback fired";
        PushState();
    }

    // Settings replay and later edits both arrive here as JSON values.
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
        (void)g_ui.RequestMenu(kViewId, true);
    }

    void OnSFSEMessage(SFSE::MessagingInterface::Message* message)
    {
        if (message->type != SFSE::MessagingInterface::kPostLoad) return;
        if (!g_ui.Init()) return;  // OSF UI is optional; degrade silently.

        // The frozen native ABI registers exact, mod-qualified names. An owning
        // view calls these through the local aliases "increment", "getState",
        // and "greet"; cross-mod callers use the qualified names below.
        g_ui.RegisterSend("__OSFUI_MOD_ID__.increment", &OnIncrement, nullptr);
        g_ui.RegisterRequest("__OSFUI_MOD_ID__.getState", &OnGetState, nullptr);
        g_ui.RegisterRequest("__OSFUI_MOD_ID__.greet", &OnGreet, nullptr);
        g_ui.RegisterRequest("__OSFUI_MOD_ID__.recalibrate", &OnRecalibrate, nullptr);
        (void)g_ui.RegisterView(kViewId);
        g_ui.SetReadyCallback(&OnBridgeAvailable, nullptr);

        if (g_ui.Has(OSFUI::API::Feature::kSettings)) {
            (void)g_ui.SubscribeSettings(kModId, &OnSetting, nullptr);
        }
        if (g_ui.Has(OSFUI::API::Feature::kHotkeys)) {
            (void)g_ui.SubscribeHotkey(kModId, "openKey", &OnHotkey, nullptr);
        }
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
