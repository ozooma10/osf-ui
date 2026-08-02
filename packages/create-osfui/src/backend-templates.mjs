const words = (value) => value.split(/[^a-zA-Z0-9]+/).filter(Boolean);

export const pascalIdentifier = (value) => {
  const joined = words(value).map((word) => word[0].toUpperCase() + word.slice(1)).join('');
  if (!joined) return 'MyMod';
  // This names a Papyrus ScriptName, which must start with a letter — a legal
  // mod id ("3dscanner.hudpanel") does not have to.
  return /^[A-Za-z]/.test(joined) ? joined : `Mod${joined}`;
};

const displayName = (modId) => words(modId.split('.')[1] || modId).join(' ') || 'My Mod';

function hudSettingsSchema(options) {
  return {
    $schema: 'https://github.com/ozooma10/osf-ui/blob/main/docs/schema/settings-schema.schema.json',
    id: options.modId,
    title: displayName(options.modId),
    description: `Controls for the ${options.view.replaceAll('-', ' ')} HUD.`,
    version: 1,
    targetVersion: '2.0.0',
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
          key: 'opacity', label: 'Opacity', type: 'float',
          min: 0.1, max: 1, step: 0.05, default: 0.9,
          format: { scale: 100, suffix: '%', decimals: 0 },
        },
      ],
    }],
  };
}

function menuSettingsSchema(options) {
  const settings = [
    { key: 'enabled', label: 'Enable backend actions', type: 'bool', default: true },
    {
      key: 'mode', label: 'Display mode', type: 'enum',
      options: ['compact', 'detailed'],
      optionLabels: ['Compact', 'Detailed'],
      default: 'detailed',
    },
    {
      key: 'intensity', label: 'Example integer', type: 'int',
      min: 0, max: 100, step: 5, default: 65,
      // A display-only predicate: the row greys out, but the value stays
      // writable and natively validated.
      enabledWhen: { key: 'enabled', eq: true },
    },
    {
      key: 'greeting', label: 'Backend greeting', type: 'string',
      default: 'Hello from OSF UI', maxLength: 80,
    },
    {
      key: 'accent', label: 'Accent colour', type: 'string', widget: 'color',
      default: '#7bdcff',
    },
    {
      key: 'openKey', label: 'Open example view', type: 'key', default: 'F9',
      allowUnbound: true,
    },
  ];
  if (options.integration === 'native') {
    // An action row dispatches a bridge REQUEST, so only a plugin that answers
    // it with RegisterRequest can serve one — Papyrus cannot.
    settings.push({
      type: 'action', key: 'recalibrate', label: 'Run native action',
      command: `${options.modId}.recalibrate`, style: 'accent',
      confirm: 'Run the generated native request example?',
    });
  }
  return {
    $schema: 'https://github.com/ozooma10/osf-ui/blob/main/docs/schema/settings-schema.schema.json',
    id: options.modId,
    title: displayName(options.modId),
    description: `Settings for ${options.view.replaceAll('-', ' ')}.`,
    version: 1,
    targetVersion: '2.0.0',
    accent: '#7bdcff',
    groups: [{ id: 'general', label: 'General', settings }],
  };
}

// The settings surface ships no view, so its schema is the whole mod: a few
// ordinary rows plus the onPress keybind the generated GLOBAL script answers.
function settingsOnlySchema(options) {
  return {
    $schema: 'https://github.com/ozooma10/osf-ui/blob/main/docs/schema/settings-schema.schema.json',
    id: options.modId,
    title: displayName(options.modId),
    description: `Settings and hotkeys for ${displayName(options.modId)}.`,
    version: 1,
    targetVersion: '2.0.0',
    groups: [{
      id: 'general',
      label: 'General',
      settings: [
        { key: 'enabled', label: 'Enabled', type: 'bool', default: true },
        {
          key: 'strength', label: 'Strength', type: 'int',
          min: 0, max: 100, step: 5, default: 50, format: { suffix: '%' },
          // Display-only: the row greys out when disabled, but Papyrus can
          // still read the value and the player's choice is preserved.
          enabledWhen: { key: 'enabled', eq: true },
        },
        {
          key: 'mode', label: 'Mode', type: 'enum',
          options: ['quiet', 'normal', 'loud'],
          optionLabels: ['Quiet', 'Normal', 'Loud'],
          default: 'normal',
        },
        {
          key: 'notifyKey', label: 'Show notification', type: 'key',
          default: 'F8', allowUnbound: true,
          hint: 'Rebindable in this menu; the binding is saved with your settings.',
          // onPress is the whole point of this template. OSF UI reads the
          // target from THIS schema at delivery time, so nothing is
          // registered and nothing is lost across a save load.
          onPress: {
            script: `${pascalIdentifier(options.modId)}OSFUI`,
            function: 'OnHotkey',
          },
        },
      ],
    }],
  };
}

export function settingsSchema(options) {
  if (options.surface === 'settings') return settingsOnlySchema(options);
  return options.surface === 'hud' ? hudSettingsSchema(options) : menuSettingsSchema(options);
}

// Standalone: this project has no package.json, so it cannot lean on
// `osfui build`. The script resolves the Creation Kit the same way
// packages/cli/src/papyrus-build.mjs does, then compiles and deploys.
const settingsBuildScript = (options) => {
  const scriptName = `${pascalIdentifier(options.modId)}OSFUI`;
  return `[CmdletBinding()]
param(
    [string]$StarfieldRoot,
    [string]$PapyrusCompiler,
    [string]$PapyrusSource,
    [string]$Mo2Mods
)

Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'

$projectRoot = $PSScriptRoot
$modRoot = Join-Path $projectRoot 'mod'
$sourceRoot = Join-Path $modRoot 'Scripts/Source'
$scriptOutput = Join-Path $modRoot 'Scripts'
$osfuiApi = Join-Path $projectRoot 'tools/papyrus'

function Resolve-StarfieldRoot {
    $steam = \${env:ProgramFiles(x86)}
    foreach ($candidate in @(
        $StarfieldRoot,
        $env:STARFIELD_ROOT,
        $env:STARFIELD_PATH,
        $(if ($steam) { Join-Path $steam 'Steam/steamapps/common/Starfield' }),
        'C:/XboxGames/Starfield/Content'
    )) {
        if ($candidate -and (Test-Path -LiteralPath $candidate -PathType Container)) { return $candidate }
    }
    return $null
}

$root = Resolve-StarfieldRoot
if (-not $PapyrusCompiler) {
    if ($env:PAPYRUS_COMPILER) {
        $PapyrusCompiler = $env:PAPYRUS_COMPILER
    } elseif ($root) {
        $PapyrusCompiler = Join-Path $root 'Tools/Papyrus Compiler/PapyrusCompiler.exe'
    }
}
if (-not $PapyrusSource) {
    if ($env:PAPYRUS_IMPORTS) {
        $PapyrusSource = $env:PAPYRUS_IMPORTS
    } elseif ($root) {
        $PapyrusSource = Join-Path $root 'Data/Scripts/Source'
    }
}

if (-not $PapyrusCompiler -or -not (Test-Path -LiteralPath $PapyrusCompiler -PathType Leaf)) {
    throw 'PapyrusCompiler.exe not found. Install the Starfield Creation Kit (Steam > Library > Tools), or pass -PapyrusCompiler.'
}
# Both files come from the Creation Kit; either one missing means the imports
# directory is not the CK script sources.
$flagsFile = Join-Path $PapyrusSource 'Starfield_Papyrus_Flags.flg'
if (-not $PapyrusSource -or
    -not (Test-Path -LiteralPath (Join-Path $PapyrusSource 'Quest.psc') -PathType Leaf) -or
    -not (Test-Path -LiteralPath $flagsFile -PathType Leaf)) {
    throw "Creation Kit script sources not found at '$PapyrusSource'. Unpack Tools/ContentResources.zip, or pass -PapyrusSource."
}

New-Item -ItemType Directory -Force -Path $scriptOutput | Out-Null
Push-Location $sourceRoot
try {
    # tools/papyrus holds the OSF UI compiler API; without it on -i the
    # OSFUI.GetBool/GetInt/GetString calls below will not resolve.
    & $PapyrusCompiler \`
        '${scriptName}.psc' \`
        "-i=$sourceRoot;$osfuiApi;$PapyrusSource" \`
        "-o=$scriptOutput" \`
        "-f=$flagsFile"
    if ($LASTEXITCODE -ne 0) { throw "Papyrus compiler exited with code $LASTEXITCODE" }
} finally {
    Pop-Location
}

Write-Host "[osfui] Compiled ${scriptName}.pex"

if (-not $Mo2Mods) {
    Write-Host '[osfui] Pass -Mo2Mods "path-to-MO2-mods" to also deploy this mod.'
    exit 0
}
$deployRoot = Join-Path $Mo2Mods '${displayName(options.modId)}'
New-Item -ItemType Directory -Force -Path $deployRoot | Out-Null
Copy-Item -Path (Join-Path $modRoot '*') -Destination $deployRoot -Recurse -Force
Write-Host "[osfui] Deployed to $deployRoot"
Write-Host '[osfui] Refresh MO2 (F5), enable the mod, load a save, close all menus, and press F8.'
`;
};

export function settingsOnlyFiles(options) {
  const scriptName = `${pascalIdentifier(options.modId)}OSFUI`;
  return [
    {
      path: `mod/SFSE/Plugins/OSFUI/settings/${options.modId}.json`,
      content: `${JSON.stringify(settingsSchema(options), null, 2)}\n`,
    },
    {
      path: `mod/Scripts/Source/${scriptName}.psc`,
      content: `ScriptName ${scriptName} Hidden
{Recordless GLOBAL script. The settings schema's onPress target names this
script and function, so OSF UI dispatches straight here — no quest, no plugin
record, no registration, and nothing to re-arm after a save load.}

; The signature must be exactly (string, string) and the function must be
; GLOBAL. Anything else and the dispatch fails, and System Health reports it
; under settings.hotkey-target:${options.modId}.notifyKey.
Function OnHotkey(string asModId, string asKey) Global
    ; Reads are cheap and always safe: an unknown key or a type mismatch
    ; returns the default you pass, so this works before the player has ever
    ; opened the menu.
    If !OSFUI.GetBool(asModId, "enabled", true)
        Return
    EndIf

    int strength = OSFUI.GetInt(asModId, "strength", 50)
    string mode = OSFUI.GetString(asModId, "mode", "normal")
    Debug.Notification("${displayName(options.modId)}: " + mode + " at " + strength + "%")
EndFunction

; Next steps:
;   - Add rows to the schema and read them here with the same typed getters.
;     OSFUI.SetBool/SetInt/SetFloat/SetString write back through the same
;     validation the menu uses.
;   - Add a second "type": "key" row with its own onPress to hand a different
;     key to a different function.
;   - To drive quest or actor state, resolve your quest lazily and start it:
;       Quest target = Game.GetFormFromFile(0x000800, "YourMod.esm") as Quest
;     0x000800 is the PLUGIN-LOCAL FormID. OSF UI never stores quest identity.
`,
    },
    {
      path: `mod/SFSE/Plugins/OSFUI/l10n/${options.modId}_de.json`,
      content: `${JSON.stringify({
        'settings.title': displayName(options.modId),
        'groups.general.label': 'Allgemein',
        'settings.enabled.label': 'Aktiviert',
        'settings.strength.label': 'Stärke',
        'settings.mode.label': 'Modus',
        'settings.mode.options.quiet': 'Leise',
        'settings.mode.options.normal': 'Normal',
        'settings.mode.options.loud': 'Laut',
        'settings.notifyKey.label': 'Benachrichtigung anzeigen',
      }, null, 2)}\n`,
    },
    { path: 'build-deploy.ps1', content: settingsBuildScript(options) },
    { path: '.gitignore', content: 'mod/Scripts/**/*.pex\n' },
  ];
}

export function settingsOnlyReadme(options) {
  const scriptName = `${pascalIdentifier(options.modId)}OSFUI`;
  return `# ${displayName(options.modId)}

A settings-only OSF UI mod: a settings page and a rebindable hotkey, with no
view code and no build tooling. Everything here is either a JSON file the game
reads directly or one Papyrus script.

- \`mod/SFSE/Plugins/OSFUI/settings/${options.modId}.json\` — the settings page
- \`mod/SFSE/Plugins/OSFUI/l10n/${options.modId}_de.json\` — a translation catalog
- \`mod/Scripts/Source/${scriptName}.psc\` — the hotkey handler
- \`tools/papyrus/OSFUI.psc\` — the OSF UI compiler API (not shipped)

## Build and deploy

Install the **Starfield Creation Kit** through Steam (Library > Tools), then:

\`\`\`powershell
./build-deploy.ps1 -Mo2Mods "C:\\path\\to\\MO2\\mods"
\`\`\`

Standard Steam and Xbox install paths are found automatically. For a portable
or nonstandard install, pass \`-StarfieldRoot\`, \`-PapyrusCompiler\`, or
\`-PapyrusSource\`. Without \`-Mo2Mods\` the script only compiles.

## Verify in game

1. Refresh MO2 (F5) and enable the mod.
2. Load a save, then press **F10** and open **${displayName(options.modId)}**.
3. Close every menu and press **F8**. A notification appears.

Hotkeys are dropped while a game menu or the console is open, so the press only
counts during gameplay. Rebind the key in the menu and the new key works
immediately.

Then **save, reload, and press it again**. This is what \`onPress\` buys you:
the target is read from the schema at delivery time, so unlike
\`OSFUI.RegisterForHotkey\` there is no registration to lose and no
\`OnPlayerLoadGame\` hook to write.

## Edit it

- **Settings page** — edit the JSON. Rows support \`bool\`, \`int\`, \`float\`,
  \`enum\`, \`flags\`, \`string\`, and \`key\` types, plus notes, images, presets,
  pages, and \`visibleWhen\`/\`enabledWhen\` predicates. Read values back with
  \`OSFUI.GetBool\` / \`GetInt\` / \`GetFloat\` / \`GetString\`. See
  [authoring-settings.md](https://github.com/ozooma10/osf-ui/blob/main/docs/authoring-settings.md).
- **Hotkey** — a \`"type": "key"\` row with an \`onPress\` target. Its
  \`script\` must match the \`ScriptName\` exactly and the function must be
  GLOBAL with an exact \`(string, string)\` signature.
- **Preview without launching Starfield** — run the OSF UI dev harness, open
  \`?view=osfui/settings\`, and drag the settings JSON onto the page.
- **Add a view later** — run \`npm create osfui@latest\` and pick the menu or
  HUD surface; the schema and script here move across unchanged.

## Ship it

Zip the contents of \`mod/\` (so \`SFSE\` and \`Scripts\` sit at the archive
root) and upload. There is no plugin to enable and no master to require. OSF UI
is the only dependency; if it is missing, every \`OSFUI.*\` call fails soft and
returns the default you passed.
`;
}

function localizationFiles(options) {
  const view = options.view;
  return [{
    path: `mod/SFSE/Plugins/OSFUI/l10n/${options.modId}_de.json`,
    content: `${JSON.stringify({
      [`views.${view}.title`]: 'OSF-UI-Starter',
      [`views.${view}.heading`]: 'OSF-UI-Starter',
      [`views.${view}.subtitle`]: 'Zustände, Ereignisse, Aktionen und Anfragen.',
      [`views.${view}.connected`]: 'Verbunden mit OSF UI {version}',
      'groups.behavior.label': 'Verhalten',
      'groups.appearance.label': 'Darstellung',
      'settings.enabled.label': 'Backend-Aktionen aktivieren',
      'settings.accent.label': 'Akzentfarbe',
    }, null, 2)}\n`,
  }];
}

function papyrusHudFiles(options) {
  const scriptName = `${pascalIdentifier(options.modId)}OSFUI`;
  return [
    {
      path: `mod/Scripts/Source/${scriptName}.psc`,
      content: `ScriptName ${scriptName} Hidden
{Recordless GLOBAL library called directly by JavaScript through OSF UI. This
loose PEX needs no quest, plugin record, or registration.}

; JavaScript: osfui.papyrus.call("${scriptName}", "Refresh")
Function Refresh() Global
    OSFUI.SetViewString("${options.modId}", "label", "SYSTEM INTEGRITY")
    OSFUI.SetViewInt("${options.modId}", "value", 72)
    OSFUI.SetViewInt("${options.modId}", "maximum", 100)
    OSFUI.SetViewString("${options.modId}", "status", "NOMINAL")
    OSFUI.SetViewBool("${options.modId}", "alert", false)
EndFunction
`,
    },
  ];
}

function papyrusFiles(options) {
  if (options.surface === 'hud') return papyrusHudFiles(options);
  const scriptName = `${pascalIdentifier(options.modId)}OSFUI`;
  return [
    {
      path: `mod/Scripts/Source/${scriptName}.psc`,
      content: `ScriptName ${scriptName} Hidden
{Recordless GLOBAL library called directly by JavaScript through OSF UI. This
loose PEX needs no quest, plugin record, or registration.}

Function Refresh() Global
    bool actionsEnabled = OSFUI.GetBool("${options.modId}", "enabled", true)
    string greeting = OSFUI.GetString("${options.modId}", "greeting", "Hello from ${scriptName}")
    OSFUI.SetViewString("${options.modId}", "greeting", greeting)
    OSFUI.SetViewInt("${options.modId}", "clicks", 0)
    OSFUI.SetViewBool("${options.modId}", "enabled", actionsEnabled)
EndFunction

; JavaScript: osfui.papyrus.call("${scriptName}", "Bump", total)
; The VIEW owns the running total and passes it in. A recordless GLOBAL script
; has nowhere to accumulate, and reading the last value back through
; OSFUI.GetInt would race this function's own queued write.
Function Bump(int total) Global
    If !OSFUI.GetBool("${options.modId}", "enabled", true)
        string[] disabledArgs = new string[1]
        disabledArgs[0] = "Backend actions are disabled in Mod Settings"
        OSFUI.SendViewEvent("${options.modId}", "notice", disabledArgs)
        Return
    EndIf
    OSFUI.SetViewInt("${options.modId}", "clicks", total)
    string[] noticeArgs = new string[1]
    noticeArgs[0] = "JavaScript called a GLOBAL Papyrus function"
    OSFUI.SendViewEvent("${options.modId}", "notice", noticeArgs)
EndFunction

Function OpenSettings() Global
    OSFUI.OpenMenu()
EndFunction

Function Greet(string who) Global
    string greeting = OSFUI.GetString("${options.modId}", "greeting", "Hello from ${scriptName}")
    OSFUI.SetViewString("${options.modId}", "greeting", greeting + ", " + who)
EndFunction

; Next steps:
;   - Real forms: OSFUI.SetViewForms publishes them as { formId, formType,
;     name }, and OSFUI.GetFormById(formId) resolves one the view echoed back.
;     Runtime FormIDs are session-scoped - check the result for None before
;     acting on it, and never store one across a save.
;   - Player-facing options belong in a settings schema and are available here
;     through OSFUI.GetBool/GetInt/GetString.
`,
    },
  ];
}

// The native build scaffolding shared by both surfaces — one copy, so the
// spot-check test on one preset covers the other too.
function nativeProjectFiles(options, description) {
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
        description = "${description}"
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
  ];
}

function nativeHudFiles(options) {
  return [
    ...nativeProjectFiles(options, 'OSF UI native HUD example'),
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
        // Retained state is replayed after every page reload and host recovery.
        (void)g_json.SetViewState(kModId, "hud", g_state);
    }

    void PushHudNotice(const char* message) noexcept
    {
        (void)g_json.SendToWeb(kViewId, "${options.modId}.notice",
            OSFUI::API::Json{ { "message", message } });
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
        if (alert) PushHudNotice("HUD entered its alert state");
    }

    void OnSFSEMessage(SFSE::MessagingInterface::Message* message)
    {
        if (message->type != SFSE::MessagingInterface::kPostLoad) return;
        if (!g_ui.Init()) return;  // OSF UI is optional; degrade silently.

        // RegisterView loads the shipped folder without editing player config.
        // The HUD manifest's openOnStart flag makes it visible immediately.
        (void)g_ui.RegisterView(kViewId);
        PushHudState();

        if (g_ui.Has(OSFUI::API::Feature::kSettings)) {
            const auto schema = OSFUI::API::Json::parse(
                R"osfui(${JSON.stringify(settingsSchema(options))})osfui", nullptr, false);
            if (!schema.is_discarded()) (void)g_json.RegisterSettingsSchema(schema);
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
    ...nativeProjectFiles(options, 'OSF UI native example'),
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

    // STATE, not a push: SetViewState retains the value and OSF UI replays it
    // to every document of this mod — first open, F5, dev reload, crash
    // recovery. The view subscribes once with osfui.state.on("<mod>/state") and is
    // never blank, and there is no "the view reloaded, re-send me everything"
    // handshake on either side. The viewId parameter is gone: state is
    // addressed by MOD, so every view of the mod gets it.
    void PushState() noexcept
    {
        (void)g_json.SetViewState(kModId, "state", g_state);
    }

    // A notice is something that HAPPENED, so it is an event: delivered once,
    // never replayed. Compare PushState below, which publishes STATE — the
    // runtime replays that to every document, including after an F5, so the
    // view never has to ask for it.
    void PushNotice(const char* viewId, const char* message) noexcept
    {
        try {
            (void)g_json.SendToWeb(viewId, kNoticeType,
                OSFUI::API::Json{ { "message", message } });
        } catch (...) {}
    }

    // A registered COMMAND is a send endpoint: one-way, nothing to settle.
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
        PushState();
    }

    // A registered REQUEST settles exactly once, with a payload or a code.
    // JavaScript: const state = await osfui.request("${options.modId}.getState")
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
            (void)request.Respond("${options.modId}.greeting", OSFUI::API::Json{
                { "message", g_state.greeting + ", " + *name + (excited ? "!!" : "!") },
                { "receivedFromJs", request.Payload() },
                { "nativeCount", g_state.count }
            });
        } catch (...) {
            request.Reject("example-error", "could not build the greeting");
        }
    }

    // Settings action rows are requests too. The built-in Mods
    // surface shows this reply message as a toast.
    void OnRecalibrate(const OSFUI::API::Request& raw, void*) noexcept
    {
        OSFUI::API::JsonRequest request{ raw };
        if (!request) return;
        g_state.count = 0;
        g_state.lastAction = "Native settings action completed";
        PushState();
        (void)request.Respond(OSFUI::API::Json{ { "message", "Example recalibration complete" } });
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
        g_ui.RegisterRequest("${options.modId}.recalibrate", &OnRecalibrate, nullptr);
        (void)g_ui.RegisterView(kViewId);
        g_ui.SetReadyCallback(&OnReady, nullptr);

        if (g_ui.Has(OSFUI::API::Feature::kSettings)) {
            const auto schema = OSFUI::API::Json::parse(
                R"osfui(${JSON.stringify(settingsSchema(options))})osfui", nullptr, false);
            if (!schema.is_discarded()) (void)g_json.RegisterSettingsSchema(schema);
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
  const shared = localizationFiles(options);
  if (options.integration === 'papyrus') {
    return [
      ...papyrusFiles(options),
      {
        path: `mod/SFSE/Plugins/OSFUI/settings/${options.modId}.json`,
        content: `${JSON.stringify(settingsSchema(options), null, 2)}\n`,
      },
      ...shared,
    ];
  }
  return [...nativeFiles(options), ...shared];
}

export function backendConfig(options) {
  if (options.integration !== 'papyrus') return '';
  return `  papyrus: {
    scriptsOnly: true,
  },
`;
}

export function backendGuide(options) {
  const scriptName = `${pascalIdentifier(options.modId)}OSFUI`;
  const papyrusBuildGuide = `The view calls GLOBAL functions on
\`${scriptName}\` directly with \`osfui.papyrus.call(script, function, ...args)\`.
The mod ships a loose PEX and needs no manifest target,
no ESM, startup quest, alias, or registration.

Run \`npm run doctor\` first. It verifies the Creation Kit compiler and source
prerequisites. Install **Starfield Creation Kit** through Steam (Library >
Tools).

Standard Steam paths and tools on \`PATH\` are discovered automatically. For
portable or nonstandard installs, add paths beside the saved \`modsRoot\` in
the ignored \`.osfui/local.json\`:

\`\`\`json
{
  "modsRoot": "D:\\\\Mod Organizer 2\\\\mods",
  "starfieldRoot": "D:\\\\SteamLibrary\\\\steamapps\\\\common\\\\Starfield"
}
\`\`\`

The first build extracts Creation Kit's \`Scripts/Source\` from
\`Tools/ContentResources.zip\` into the ignored \`.osfui/\` cache. The matching
OSF UI compiler API is pinned at \`tools/papyrus/OSFUI.psc\`.
`;
  if (options.surface === 'hud' && options.integration === 'papyrus') {
    return `## Papyrus HUD backend

The generated page calls the recordless GLOBAL library for a fresh demo
snapshot whenever its document is created. Replace the \`Refresh\` function with
the game operations and \`OSFUI.SetView*\` values your HUD needs. The page owns
the standard HUD behavior: it subscribes to the generated settings schema,
applies anchor and opacity changes live, and handles the rebindable F8 toggle
without capturing input. \`osfui.mock.ts\` mirrors all of it in the browser.

${papyrusBuildGuide}
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
input. It applies anchor and opacity settings live and handles
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

1. Run \`npm run doctor\` and install any missing Creation Kit prerequisites.
2. Run \`npm run build\` to compile the loose PEX and build the view.
3. Run \`npm run package\` to create the plugin-free installable zip in
   \`release/\`.

## Debug

- Run \`npm run dev\` to test the view in a browser with hot reload. Edit
  \`osfui.mock.ts\` to provide test Papyrus data and responses.
- Run \`npm run dev:game -- --deploy "path-to-MO2-mods"\` to test in Starfield.
  Loaded views reload automatically; press F12 to open DevTools.

The Papyrus library is
\`mod/Scripts/Source/${scriptName}.psc\`. Its compiled PEX is discovered
on demand when JavaScript calls one of its GLOBAL functions; there is no plugin
to enable and no ESM, startup quest, alias, or registration to maintain.
`,
    native: `## Native SFSE backend

The paired \`native/src/main.cpp\` and view source are an end-to-end bridge
example built on the optional \`OSFUI_JSON.h\` facade:

- **Send command to C++** sends a typed fire-and-forget \`JsonCommand\`; C++
  changes its state and pushes the serialized struct back to JavaScript.
- **Call C++ and await reply** sends a \`JsonRequest\`; C++ validates the
  required \`name\`, replies with JSON, and lets OSF UI own correlation.
- The plugin also registers this view, a runtime settings schema, settings and
  ready callbacks, and an **F9** open-view hotkey. \`osfui.mock.ts\` mirrors
  the round trips in the browser harness.

1. Install xmake and Visual Studio's C++ workload.
2. Add CommonLibSF: \`git submodule add https://github.com/ozooma10/commonlibsf.git native/lib/commonlibsf\`.
3. Run \`npm run build:native\`. xmake fetches nlohmann/json and puts the DLL in \`mod/SFSE/Plugins/\`.
4. Run \`npm run package\` to build the DLL and view into one mod archive.
`,
  };
  return guides[options.integration];
}

// The generated project is a starter, not a catalogue — everything it does not
// demonstrate is documented, so point at the documentation rather than growing
// the template.
export function docsGuide(options) {
  const docs = 'https://github.com/ozooma10/osf-ui/blob/main/docs';
  return `## Where to read more

- [authoring-views.md](${docs}/authoring-views.md) — the full bridge protocol:
  every platform endpoint, event, and lifecycle rule.
- [authoring-settings.md](${docs}/authoring-settings.md) — every settings
  control, widget, predicate, preset, and localization address.
- [authoring-dynamic-data.md](${docs}/authoring-dynamic-data.md) — a worked
  state-and-event example between a backend and a view.
${options.integration === 'native'
    ? `- [native-plugin-api.md](${docs}/native-plugin-api.md) and the copied
  \`native/include/OSFUI_API.h\` — the complete C ABI.`
    : `- The copied \`tools/papyrus/OSFUI.psc\` — every OSF UI Papyrus function
  with its contract in the comments.`}
- [view-toolchain.md](${docs}/view-toolchain.md) — the CLI, the browser
  harness, deployment, and packaging.
`;
}
