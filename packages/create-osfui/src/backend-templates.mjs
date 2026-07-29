const words = (value) => value.split(/[^a-zA-Z0-9]+/).filter(Boolean);

export const pascalIdentifier = (value) =>
  words(value).map((word) => word[0].toUpperCase() + word.slice(1)).join('') || 'MyMod';

const displayName = (modId) => words(modId.split('.')[1] || modId).join(' ') || 'My Mod';

function papyrusFiles(options) {
  const scriptName = `${pascalIdentifier(options.modId)}OSFUI`;
  const aliasName = `${scriptName}PlayerAlias`;
  return [
    {
      path: `mod/Scripts/Source/User/${scriptName}.psc`,
      content: `ScriptName ${scriptName} Extends Quest
{OSF UI backend. Attach to a Start Game Enabled quest, and attach
${aliasName} to a player reference alias on the same quest.}

string ModId = "${options.modId}"
int actionToken = 0
int requestToken = 0
int clicks = 0

Event OnInit()
    RegisterOSFUI()
EndEvent

; Registrations AND published state are session-scoped: neither survives a save
; load, so ${aliasName} calls this again from OnPlayerLoadGame.
Function RegisterOSFUI()
    ; GetVersion() is the installation check - 0 means OSF UI is not installed.
    If OSFUI.GetVersion() == 0
        Return
    EndIf

    ; Drop last session's tokens (a no-op when they are already stale).
    OSFUI.Unregister(actionToken)
    OSFUI.Unregister(requestToken)

    actionToken = OSFUI.ListenForViewActions(self as ScriptObject, ModId)
    requestToken = OSFUI.ListenForViewRequests(self as ScriptObject, ModId)
    ; A 0 token means the registration was refused - usually because another
    ; script already listens for this mod id (the first listener wins).
    If actionToken == 0 || requestToken == 0
        Debug.Trace("[" + ModId + "] OSF UI registration failed: actions=" + actionToken + " requests=" + requestToken, 2)
    EndIf

    PublishState()
EndFunction

; State drives the view. Each SetView* call replaces the cached value for
; (ModId, key), reaches every live view of this mod, and is replayed whenever
; one opens or reloads - no ready handshake. The cache is session-scoped, so
; publish again after a game load.
Function PublishState()
    OSFUI.SetViewString(ModId, "greeting", "Hello from ${scriptName}")
    OSFUI.SetViewInt(ModId, "clicks", clicks)
EndFunction

; One-way player actions: the view fires osfui.action("bump", 1). Change game
; state here and publish the result - an action never sends a reply.
Function OnOSFUIViewAction(string action, string[] args)
    If action == "bump"
        int amount = 1
        If args.Length > 0
            amount = args[0] as int
        EndIf
        clicks += amount
        PublishState()
    ElseIf action == "openSettings"
        OSFUI.OpenMenu()    ; the Mods surface, same as F10
    EndIf
EndFunction

; Value-returning operations: the view awaits osfui.papyrus.request("greet", name).
; Answer EXACTLY ONCE with a ReplyView* or RejectViewRequest - the reply token
; is one-shot and expires after ten seconds.
Function OnOSFUIViewRequest(string request, string[] args, string replyToken)
    If request == "greet"
        string who = ""
        If args.Length > 0
            who = args[0]
        EndIf
        If who == ""
            ; The code arrives in JavaScript as error.code on the rejection.
            OSFUI.RejectViewRequest(replyToken, "invalid-name", "Type a name first")
            Return
        EndIf
        OSFUI.ReplyViewString(replyToken, "Hello " + who + ", from ${scriptName}")
    Else
        OSFUI.RejectViewRequest(replyToken, "unknown-request", request)
    EndIf
EndFunction

; Next steps:
;   - Real forms: OSFUI.SetViewForms publishes them as { formId, formType,
;     name }, and OSFUI.GetFormById(args[0]) resolves one the view echoed back.
;     Runtime FormIDs are session-scoped - check the result for None before
;     acting on it, and never store one across a save.
;   - Player-facing options belong in a settings schema (OSFUI.GetBool/GetInt/
;     GetString plus RegisterForSettingChanges), not in view state.
`,
    },
    {
      path: `mod/Scripts/Source/User/${aliasName}.psc`,
      content: `ScriptName ${aliasName} Extends ReferenceAlias
{Attach to a player reference alias on the same quest as ${scriptName}.
OSF UI registrations and published state are session-scoped: without this the
view stops responding once the player loads a save.}

Event OnPlayerLoadGame()
    ${scriptName} owner = GetOwningQuest() as ${scriptName}
    If owner != None
        owner.RegisterOSFUI()
    EndIf
EndEvent
`,
    },
  ];
}

function nativeFiles(options) {
  const pluginName = pascalIdentifier(options.modId);
  return [
    {
      path: 'xmake.lua',
      content: `set_project("${pluginName}")
set_version("0.1.0")
set_arch("x64")
set_languages("c++23")
add_requires("nlohmann_json")

add_rules("mode.debug", "mode.releasedbg")

includes("native/lib/commonlibsf")

target("${pluginName}")
    add_rules("commonlibsf.plugin", {
        name = "${displayName(options.modId)}",
        author = "${options.modId.split('.')[0]}",
        description = "OSF UI native example"
    })
    add_files("native/src/**.cpp")
    add_headerfiles("native/include/**.h")
    add_includedirs("native/include")
    set_installdir("mod")
    add_packages("nlohmann_json")
`,
    },
    {
      path: 'native/build.mjs',
      content: `import { spawn } from 'node:child_process';
import { dirname, resolve } from 'node:path';
import { fileURLToPath } from 'node:url';

const projectRoot = resolve(dirname(fileURLToPath(import.meta.url)), '..');

const env = { ...process.env };
delete env.XSE_SF_MODS_PATH;
delete env.XSE_SF_GAME_PATH;

const code = await new Promise((resolve, reject) => {
  const child = spawn('xmake', ['build', '-P', projectRoot], { env, stdio: 'inherit' });
  child.once('error', reject);
  child.once('exit', resolve);
});
if (code !== 0) process.exit(code ?? 1);
`,
    },
    {
      path: 'native/src/main.cpp',
      content: `#include <SFSE/SFSE.h>
#include <algorithm>
#include <cstring>
#include <string>
#include "OSFUI_JSON.h"

namespace
{
    OSFUI::API::Client g_ui;
    OSFUI::API::JsonClient g_json{ g_ui };

    constexpr const char* kModId = "${options.modId}";
    constexpr const char* kViewId = "${options.modId}/${options.view}";
    constexpr const char* kStateType = "${options.modId}.state";
    constexpr const char* kNoticeType = "${options.modId}.notice";

    struct DemoState
    {
        int count{ 0 };
        bool enabled{ true };
        std::string greeting{ "Hello from the ${pluginName} native plugin" };
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
                "typed JSON", "commands", "requests", "native pushes", "settings", "hotkeys"
            }) }
        };
    }

    DemoState g_state;

    void PushState(const char* viewId = kViewId) noexcept
    {
        (void)g_json.SendToWeb(viewId, kStateType, g_state);
    }

    void PushNotice(const char* viewId, const char* message) noexcept
    {
        try {
            (void)g_json.SendToWeb(viewId, kNoticeType,
                OSFUI::API::Json{ { "message", message } });
        } catch (...) {}
    }

    // JavaScript: osfui.send("${options.modId}.increment", { amount: 1 })
    void OnIncrement(const char* command, const char* payloadJson,
        const char* sourceViewId, void*) noexcept
    {
        OSFUI::API::JsonCommand event{ command, payloadJson, sourceViewId };
        const char* target = event.SourceViewId().empty() ? kViewId : event.SourceViewId().data();
        if (!event) {
            PushNotice(target, "C++ ignored a malformed command payload");
            return;
        }
        if (!g_state.enabled) {
            PushNotice(target, "The native counter is disabled in Mod Settings");
            return;
        }

        g_state.count += std::clamp(event.Value("amount", 1), -10, 10);
        g_state.lastAction = "JavaScript sent a fire-and-forget command";
        PushState(target);
    }

    // JavaScript: const state = await osfui.call("${options.modId}.getState")
    void OnGetState(const OSFUI::API::Request& raw, void*) noexcept
    {
        OSFUI::API::JsonRequest request{ raw };
        if (request) (void)request.Respond(kStateType, g_state);
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
            (void)request.Respond("${options.modId}.greeting", OSFUI::API::Json{
                { "message", g_state.greeting + ", " + *name + (excited ? "!!" : "!") },
                { "receivedFromJs", request.Payload() },
                { "nativeCount", g_state.count }
            });
        } catch (...) {
            request.Reject("example-error", "could not build the greeting");
        }
    }

    // Main-thread callback fired when a bridge becomes live (and after recreation).
    void OnReady(void*) noexcept
    {
        g_state.lastAction = "OSF UI ready callback fired";
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

        g_ui.RegisterCommand("${options.modId}.increment", &OnIncrement, nullptr);
        g_ui.RegisterRequest("${options.modId}.getState", &OnGetState, nullptr);
        g_ui.RegisterRequest("${options.modId}.greet", &OnGreet, nullptr);
        (void)g_ui.RegisterView(kViewId);
        g_ui.SetReadyCallback(&OnReady, nullptr);

        if (g_ui.Has(OSFUI::API::Feature::kSettings)) {
            (void)g_json.RegisterSettingsSchema(OSFUI::API::Json{
                { "id", kModId },
                { "title", "${displayName(options.modId)} native example" },
                { "description", "Runtime schema registered from C++ with OSFUI_JSON." },
                { "version", 1 },
                { "groups", OSFUI::API::Json::array({
                    OSFUI::API::Json{
                        { "id", "native-demo" },
                        { "label", "Native bridge demo" },
                        { "settings", OSFUI::API::Json::array({
                            OSFUI::API::Json{
                                { "key", "enabled" }, { "label", "Enable native counter" },
                                { "type", "bool" }, { "default", true }
                            },
                            OSFUI::API::Json{
                                { "key", "greeting" }, { "label", "Greeting prefix" },
                                { "type", "string" }, { "default", g_state.greeting },
                                { "maxLength", 80 }
                            },
                            OSFUI::API::Json{
                                { "key", "openKey" }, { "label", "Open example view" },
                                { "type", "key" }, { "default", "F9" }, { "allowUnbound", true }
                            }
                        }) }
                    }
                }) }
            });
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
`,
    },
  ];
}

export function backendFiles(options) {
  if (options.integration === 'papyrus') return papyrusFiles(options);
  return nativeFiles(options);
}

export function backendGuide(options) {
  const scriptName = `${pascalIdentifier(options.modId)}OSFUI`;
  const aliasName = `${scriptName}PlayerAlias`;
  const guides = {
    papyrus: `## Papyrus backend

\`mod/Scripts/Source/User/${scriptName}.psc\` covers all three ways a script
talks to a view, and the generated page exercises each one:

- **State** — \`OSFUI.SetView*\` publishes the counter and greeting; the page
  reads them with \`osfui.data.on(key, fn)\`. Values are cached and replayed on
  every view reload, so there is no handshake. Use this for anything rendered.
- **Action** — the buttons fire \`osfui.action(name, ...args)\`, which arrives as
  \`OnOSFUIViewAction\`. The script mutates state and publishes it again.
- **Request** — the form awaits \`osfui.papyrus.request("greet", name)\`, which
  arrives as \`OnOSFUIViewRequest\` and is answered once with \`ReplyViewString\`
  (or \`RejectViewRequest\`, whose code becomes \`error.code\` in JavaScript).
  Reserve requests for operations whose *result* is the point.

\`osfui.mock.ts\` stands in for the script so all of it works in the
browser harness before Starfield is involved.

1. In the Creation Kit, create a Start Game Enabled quest and attach
   \`${scriptName}\` to it.
2. Add a reference alias on that quest filled with the player, and attach
   \`${aliasName}\`. Registrations and published state are
   session-scoped — without this alias the view goes silent after a save load.
3. Compile both scripts with Starfield's base sources and OSF UI's \`OSFUI.psc\`
   available, placing the \`.pex\` files under \`mod/Scripts/\`.
4. Run \`npm run package\`. The compiled scripts, sources, and view are included.
`,
    native: `## Native SFSE backend

The paired \`native/src/main.cpp\` and view source are an end-to-end bridge
example built on the optional \`OSFUI_JSON.h\` facade:

- **Send command to C++** sends a typed fire-and-forget \`JsonCommand\`; C++
  changes its state and pushes the serialized struct back to JavaScript.
- **Call C++ and await reply** sends a \`JsonRequest\`; C++ validates the
  required \`name\`, replies with JSON, and lets OSF UI own correlation.
- The plugin registers this view, a runtime settings schema, settings/ready
  callbacks, and an **F9** open-view hotkey. Edit the generated code down to
  the pieces your mod needs. \`osfui.mock.ts\` mirrors the round trips in the
  browser harness and exposes settings/hotkey callback controls in its toolbar.

1. Install xmake and Visual Studio's C++ workload.
2. Add CommonLibSF: \`git submodule add https://github.com/ozooma10/commonlibsf.git native/lib/commonlibsf\`.
3. Run \`npm run build:native\`. xmake fetches nlohmann/json and puts the DLL in \`mod/SFSE/Plugins/\`.
4. Run \`npm run package\` to build the DLL and view into one mod archive.
`,
  };
  return guides[options.integration];
}
