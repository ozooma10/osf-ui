const words = (value) => value.split(/[^a-zA-Z0-9]+/).filter(Boolean);

export const pascalIdentifier = (value) => {
  const joined = words(value).map((word) => word[0].toUpperCase() + word.slice(1)).join('');
  if (!joined) return 'MyMod';
  // This names a Papyrus ScriptName and a quest EditorID, which must start
  // with a letter — a legal mod id ("3dscanner.hudpanel") does not have to.
  return /^[A-Za-z]/.test(joined) ? joined : `Mod${joined}`;
};

const displayName = (modId) => words(modId.split('.')[1] || modId).join(' ') || 'My Mod';

function papyrusPluginFiles(options) {
  const pluginName = `${pascalIdentifier(options.modId)}.esm`;
  const scriptName = `${pascalIdentifier(options.modId)}OSFUI`;
  const aliasName = `${scriptName}PlayerAlias`;
  const questName = `${scriptName}Quest`;
  const sourceRoot = `spriggit/${pluginName}`;
  const questRoot = `${sourceRoot}/Quests/${questName} - 000800_${pluginName}`;
  return [
    {
      path: '.spriggit',
      content: `${JSON.stringify({
        KnownMasters: [{ ModKey: 'Starfield.esm', Style: 'Full' }],
      }, null, 2)}\n`,
    },
    {
      path: `${sourceRoot}/spriggit-meta.json`,
      content: `${JSON.stringify({
        PackageName: 'Spriggit.Yaml.Starfield',
        Version: '0.35.1',
        Release: 'Starfield',
        ModKey: pluginName,
      }, null, 2)}\n`,
    },
    {
      path: `${sourceRoot}/RecordData.yaml`,
      content: `SpriggitSource:
  PackageName: Spriggit.Yaml.Starfield
  Version: 0.35.1
ModKey: ${pluginName}
GameRelease: Starfield
ModHeader:
  Flags:
  - Master
  MasterReferences:
  - Master: Starfield.esm
`,
    },
    {
      path: `${questRoot}/RecordData.yaml`,
      content: `FormKey: 000800:${pluginName}
EditorID: ${questName}
VirtualMachineAdapter:
  Scripts:
  - Name: ${scriptName}
  Script:
    Name: ''
  Aliases:
  - Property:
      Name: ''
      Object: 000800:${pluginName}
      Alias: 0
    Scripts:
    - Name: ${aliasName}
Name:
  TargetLanguage: English
  Value: ${displayName(options.modId)} OSF UI backend
Data:
  Flags:
  - StartGameEnabled
  - RunOnce
  Priority: 50
  Unused: 0x000000
Aliases:
- MutagenObjectType: QuestReferenceAlias
  Name: Player
  Flags:
  - AllowReserved
  ForcedReference: 000014:Starfield.esm
`,
    },
  ];
}

function hudSettingsSchema(options) {
  return {
    $schema: 'https://github.com/ozooma10/osf-ui/blob/main/docs/schema/settings-schema.schema.json',
    id: options.modId,
    title: displayName(options.modId),
    description: `Controls for the ${options.view.replaceAll('-', ' ')} HUD.`,
    version: 1,
    groups: [{
      id: 'hud',
      label: 'HUD',
      settings: [
        { key: 'hudEnabled', label: 'Show HUD', type: 'bool', default: true },
        {
          key: 'toggleHud', label: 'Toggle HUD', type: 'key', default: 'F8',
          allowUnbound: true, hint: 'Shows or hides the HUD during gameplay.',
        },
        {
          key: 'anchor', label: 'Screen position', type: 'enum',
          options: ['top-left', 'top-right', 'bottom-left', 'bottom-right'],
          optionLabels: ['Top left', 'Top right', 'Bottom left', 'Bottom right'],
          default: 'top-right',
        },
        {
          key: 'margin', label: 'Screen margin', type: 'int',
          min: 0, max: 160, step: 4, default: 32, format: { suffix: ' px' },
        },
        {
          key: 'scale', label: 'Scale', type: 'int',
          min: 50, max: 200, step: 5, default: 100, format: { suffix: '%' },
        },
        {
          key: 'opacity', label: 'Opacity', type: 'float',
          min: 0.1, max: 1, step: 0.05, default: 0.9,
          format: { scale: 100, suffix: '%', decimals: 0 },
        },
        {
          key: 'accent', label: 'Accent colour', type: 'string',
          widget: 'color', default: '#7bdcff',
        },
      ],
    }],
  };
}

function papyrusHudFiles(options) {
  const scriptName = `${pascalIdentifier(options.modId)}OSFUI`;
  const aliasName = `${scriptName}PlayerAlias`;
  const viewId = `${options.modId}/${options.view}`;
  return [
    {
      path: `mod/Scripts/Source/User/${scriptName}.psc`,
      content: `ScriptName ${scriptName} Extends Quest
{Passive HUD backend. Attach to a Start Game Enabled quest, and attach
${aliasName} to a player reference alias on the same quest.}

string ModId = "${options.modId}"
string ViewId = "${viewId}"

int hudValue = 72
int hudMaximum = 100
string hudLabel = "SYSTEM INTEGRITY"
string hudStatus = "NOMINAL"
bool hudAlert = false

Event OnInit()
    RegisterOSFUI()
EndEvent

; Cached view state is session-scoped, so ${aliasName} calls this again after
; every save load. OpenMenu also loads this discovered HUD on first use.
Function RegisterOSFUI()
    If OSFUI.GetVersion() == 0
        Return
    EndIf

    PublishHUD()
    OSFUI.OpenMenu(ViewId)
EndFunction

; Call this from your gameplay events when the displayed data changes. HUDs
; should be event-driven; avoid publishing unchanged values every frame.
Function UpdateHUD(int aiValue, int aiMaximum, string asStatus, bool abAlert)
    hudValue = aiValue
    hudMaximum = aiMaximum
    hudStatus = asStatus
    hudAlert = abAlert
    PublishHUD()
EndFunction

; Each value is cached by (ModId, key), pushed to every live owning view, and
; replayed automatically after a page reload. Publish again after a save load.
Function PublishHUD()
    OSFUI.SetViewString(ModId, "label", hudLabel)
    OSFUI.SetViewInt(ModId, "value", hudValue)
    OSFUI.SetViewInt(ModId, "maximum", hudMaximum)
    OSFUI.SetViewString(ModId, "status", hudStatus)
    OSFUI.SetViewBool(ModId, "alert", hudAlert)
EndFunction
`,
    },
    {
      path: `mod/Scripts/Source/User/${aliasName}.psc`,
      content: `ScriptName ${aliasName} Extends ReferenceAlias
{Attach to a player reference alias on the same quest as ${scriptName}.
OSF UI view state is session-scoped, so the quest republishes after save loads.}

Event OnPlayerLoadGame()
    ${scriptName} owner = GetOwningQuest() as ${scriptName}
    If owner != None
        owner.RegisterOSFUI()
    EndIf
EndEvent
`,
    },
    {
      path: `mod/SFSE/Plugins/OSFUI/settings/${options.modId}.json`,
      content: `${JSON.stringify(hudSettingsSchema(options), null, 2)}\n`,
    },
  ];
}

function papyrusFiles(options) {
  if (options.surface === 'hud') return papyrusHudFiles(options);
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
; The parameter cannot be named "action" - that is the Action form type in
; Papyrus, and references to it resolve to the type, not the parameter.
Function OnOSFUIViewAction(string actionName, string[] args)
    If actionName == "bump"
        int amount = 1
        If args.Length > 0
            amount = args[0] as int
        EndIf
        clicks += amount
        PublishState()
    ElseIf actionName == "openSettings"
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

function nativeHudFiles(options) {
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
        description = "OSF UI native HUD example"
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
#include <string>
#include <utility>
#include "OSFUI_JSON.h"

namespace
{
    OSFUI::API::Client g_ui;
    OSFUI::API::JsonClient g_json{ g_ui };

    constexpr const char* kModId = "${options.modId}";
    constexpr const char* kViewId = "${options.modId}/${options.view}";
    constexpr const char* kStateType = "${options.modId}.hudState";

    struct HudState
    {
        int value{ 72 };
        int maximum{ 100 };
        std::string label{ "SYSTEM INTEGRITY" };
        std::string status{ "NOMINAL" };
        bool alert{ false };
    };

    void to_json(OSFUI::API::Json& json, const HudState& state)
    {
        json = {
            { "value", state.value },
            { "maximum", state.maximum },
            { "label", state.label },
            { "status", state.status },
            { "alert", state.alert }
        };
    }

    HudState g_state;

    void PushHudState() noexcept
    {
        (void)g_json.SendToWeb(kViewId, kStateType, g_state);
    }

    // Call this from your game-event handling when displayed data changes.
    // Prefer changed snapshots or a bounded cadence over sending every frame.
    void UpdateHudState(int value, int maximum, std::string status, bool alert)
    {
        g_state.value = value;
        g_state.maximum = maximum;
        g_state.status = std::move(status);
        g_state.alert = alert;
        PushHudState();
    }

    // Main-thread callback fired when a bridge becomes live and after recovery.
    void OnReady(void*) noexcept
    {
        PushHudState();
    }

    void OnSFSEMessage(SFSE::MessagingInterface::Message* message)
    {
        if (message->type != SFSE::MessagingInterface::kPostLoad) return;
        if (!g_ui.Init()) return;  // OSF UI is optional; degrade silently.

        // RegisterView loads the shipped folder without editing player config.
        // The HUD manifest's openOnStart flag makes it visible immediately.
        (void)g_ui.RegisterView(kViewId);
        g_ui.SetReadyCallback(&OnReady, nullptr);

        if (g_ui.Has(OSFUI::API::Feature::kSettings)) {
            (void)g_json.RegisterSettingsSchema(OSFUI::API::Json{
                { "id", kModId },
                { "title", "${displayName(options.modId)}" },
                { "description", "Controls for the ${options.view.replaceAll('-', ' ')} HUD." },
                { "version", 1 },
                { "groups", OSFUI::API::Json::array({
                    OSFUI::API::Json{
                        { "id", "hud" },
                        { "label", "HUD" },
                        { "settings", OSFUI::API::Json::array({
                            OSFUI::API::Json{
                                { "key", "hudEnabled" }, { "label", "Show HUD" },
                                { "type", "bool" }, { "default", true }
                            },
                            OSFUI::API::Json{
                                { "key", "toggleHud" }, { "label", "Toggle HUD" },
                                { "type", "key" }, { "default", "F8" },
                                { "allowUnbound", true },
                                { "hint", "Shows or hides the HUD during gameplay." }
                            },
                            OSFUI::API::Json{
                                { "key", "anchor" }, { "label", "Screen position" },
                                { "type", "enum" },
                                { "options", OSFUI::API::Json::array({
                                    "top-left", "top-right", "bottom-left", "bottom-right"
                                }) },
                                { "optionLabels", OSFUI::API::Json::array({
                                    "Top left", "Top right", "Bottom left", "Bottom right"
                                }) },
                                { "default", "top-right" }
                            },
                            OSFUI::API::Json{
                                { "key", "margin" }, { "label", "Screen margin" },
                                { "type", "int" }, { "min", 0 }, { "max", 160 },
                                { "step", 4 }, { "default", 32 },
                                { "format", OSFUI::API::Json{ { "suffix", " px" } } }
                            },
                            OSFUI::API::Json{
                                { "key", "scale" }, { "label", "Scale" },
                                { "type", "int" }, { "min", 50 }, { "max", 200 },
                                { "step", 5 }, { "default", 100 },
                                { "format", OSFUI::API::Json{ { "suffix", "%" } } }
                            },
                            OSFUI::API::Json{
                                { "key", "opacity" }, { "label", "Opacity" },
                                { "type", "float" }, { "min", 0.1 }, { "max", 1.0 },
                                { "step", 0.05 }, { "default", 0.9 },
                                { "format", OSFUI::API::Json{
                                    { "scale", 100 }, { "suffix", "%" }, { "decimals", 0 }
                                } }
                            },
                            OSFUI::API::Json{
                                { "key", "accent" }, { "label", "Accent colour" },
                                { "type", "string" }, { "widget", "color" },
                                { "default", "#7bdcff" }
                            }
                        }) }
                    }
                }) }
            });
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

function nativeFiles(options) {
  if (options.surface === 'hud') return nativeHudFiles(options);
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
  if (options.integration === 'papyrus') {
    return [...papyrusFiles(options), ...papyrusPluginFiles(options)];
  }
  return nativeFiles(options);
}

export function backendConfig(options) {
  if (options.integration !== 'papyrus') return '';
  const pluginName = `${pascalIdentifier(options.modId)}.esm`;
  return `  papyrus: {
    plugin: '${pluginName}',
    source: 'spriggit/${pluginName}',
  },
`;
}

export function backendGuide(options) {
  const scriptName = `${pascalIdentifier(options.modId)}OSFUI`;
  const aliasName = `${scriptName}PlayerAlias`;
  const pluginName = `${pascalIdentifier(options.modId)}.esm`;
  const papyrusBuildGuide = `The project is a complete mod rather than loose
example scripts. \`spriggit/${pluginName}/\` defines the Start Game Enabled
quest, its backend script, and the player alias that restores registrations
after a save load.

\`npm run build\`, \`npm run package\`, and every \`dev:game\` sync generate
\`mod/${pluginName}\` with Spriggit and compile the scripts with Creation Kit.
The release zip is loadable as-is once its ESM is enabled in the mod manager.

Run \`npm run doctor\` first. It verifies every prerequisite and explains any
missing install. Install **Starfield Creation Kit** through Steam (Library >
Tools), and download **SpriggitCLI.zip** from
https://github.com/Mutagen-Modding/Spriggit/releases.

Standard Steam paths and tools on \`PATH\` are discovered automatically. For
portable or nonstandard installs, add paths beside the saved \`modsRoot\` in
the ignored \`.osfui/local.json\`:

\`\`\`json
{
  "modsRoot": "D:\\\\Mod Organizer 2\\\\mods",
  "starfieldRoot": "D:\\\\SteamLibrary\\\\steamapps\\\\common\\\\Starfield",
  "spriggitCli": "D:\\\\Tools\\\\Spriggit\\\\Spriggit.CLI.exe"
}
\`\`\`

The first build extracts Creation Kit's \`Scripts/Source\` from
\`Tools/ContentResources.zip\` into the ignored \`.osfui/\` cache. The matching
OSF UI compiler API is pinned at \`tools/papyrus/OSFUI.psc\`.

If you edit the quest in Creation Kit, serialize it back before rebuilding:

\`\`\`powershell
Spriggit.CLI.exe serialize --InputPath "mod/${pluginName}" --OutputPath "spriggit/${pluginName}" --GameRelease Starfield --PackageName Spriggit.Yaml --PackageVersion 0.35.1 --DataFolder "path-to-Starfield/Data"
\`\`\`
`;
  if (options.surface === 'hud' && options.integration === 'papyrus') {
    return `## Papyrus HUD backend

The generated quest script is an event-driven HUD producer. It publishes a
typed snapshot with \`OSFUI.SetView*\`; OSF UI caches every field and replays it
when the HUD page opens or reloads. Call \`UpdateHUD(...)\` from the gameplay
events that change the display rather than polling or publishing every frame.

The script opens the discovered \`${options.modId}/${options.view}\` surface at
startup, and the player-alias companion republishes and reopens it after each
save load. The page itself owns the standard HUD behavior: it subscribes to
the generated settings schema, applies anchor/margin/scale/opacity/accent
changes live, and handles the rebindable F8 toggle without capturing input.
\`osfui.mock.ts\` provides telemetry, alert, appearance, and hotkey controls.

${papyrusBuildGuide}
Replace the demo fields in \`UpdateHUD\` with your real event data.
`;
  }
  if (options.surface === 'hud' && options.integration === 'native') {
    return `## Native SFSE HUD backend

The generated plugin registers and auto-opens its shipped HUD, publishes a
typed \`HudState\` snapshot on bridge startup/recovery, and registers the
standard HUD settings schema at runtime. Call \`UpdateHudState(...)\` from your
game-event handling when displayed values change; avoid per-frame bridge
traffic when a changed snapshot or bounded cadence will do.

The passive full-screen page has no controls and never captures gameplay
input. It applies anchor/margin/scale/opacity/accent settings live and handles
the rebindable F8 visibility toggle. \`osfui.mock.ts\` mirrors native state,
alerts, settings, and the hotkey in the browser harness.

1. Install xmake and Visual Studio's C++ workload.
2. Add CommonLibSF: \`git submodule add https://github.com/ozooma10/commonlibsf.git native/lib/commonlibsf\`.
3. Run \`npm run build:native\`. xmake fetches nlohmann/json and puts the DLL
   in \`mod/SFSE/Plugins/\`.
4. Replace the demo state with your game data, then run \`npm run package\` to
   build the DLL and HUD into one mod archive.
`;
  }
  const guides = {
    papyrus: `## Build

1. Run \`npm run doctor\` and install any missing Creation Kit or Spriggit
   prerequisites it reports.
2. Run \`npm run build\` to generate the plugin, compile Papyrus, and build the
   view.
3. Run \`npm run package\` to create the installable zip in \`release/\`.

## Debug

- Run \`npm run dev\` to test the view in a browser with hot reload. Edit
  \`osfui.mock.ts\` to provide test Papyrus data and responses.
- Run \`npm run dev:game -- --deploy "path-to-MO2-mods"\` to test in Starfield.
  Loaded views reload automatically; press F12 to open DevTools.

The Papyrus backend is
\`mod/Scripts/Source/User/${scriptName}.psc\`.
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
