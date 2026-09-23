# Settings → native hotkey → WebView

Development-only consumer for OSF Settings Slim and OSF UI 2.0. It is not built
or packaged by the production UI target.

Slim owns the `osfui-example` schema and its native **Open example panel** hotkey
(F6 by default; rebind through Settings). The plugin acquires both SDKs at
`kPostPostLoad`, subscribes before reading `showDetails`, and publishes only that
boolean through `SetViewState`. The hotkey callback queues `RequestMenu`; it does
not call the engine. All registrations and their owner live for the game process.
The panel consumes ordinary retained state and closes with its button or Escape.
It includes a text field and slider for testing keyboard, mouse, and focus behavior.
Its manifest also opts into the shared Launcher tab under OSF UI Example. Opening
uses the normal RequestMenu preflight/runtime pipeline. The view owns closing;
reopen Settings through its normal hotkey or menu entry. Include that cycle in
the acceptance run below.

## Build and stage on Windows

From the OSF UI repository root with initialized submodules and MSVC/XMake:

```powershell
# Keep the development example out of the ordinary game/MO2 deployment.
$env:XSE_SF_MODS_PATH = $null
$env:XSE_SF_GAME_PATH = $null
xmake f -P examples/settings-view -y -m releasedbg
xmake build -P examples/settings-view -y
xmake install -P examples/settings-view -o out/settings-view-example -y "OSF UI Settings Example"
```

Install the staged contents as a separate test mod alongside the OSF UI and
OSF Settings Slim packages in the isolated OSF Testing profile. The example
needs the production UI package's shared `osfui.js`; it does not carry its own
copy. The schema must be installed before starting Starfield.

For a local MO2 test, install the staged contents as a separate mod named
`OSF UI Settings Example`, enable it in MO2, then start Starfield. Press F6 to
open the panel. OSF UI itself has no built-in panel or open hotkey.

## Acceptance

1. Start with **Show details** off. Open the panel using F6; details are hidden.
2. Close it, change the setting through OSF Settings, and reopen. Details appear.
3. Rebind the hotkey through Settings, restart the game, and use the new binding.
4. While the panel captures input, registered gameplay hotkeys stay blocked.
   Close or cancel the panel and verify hotkeys work again.
5. Cause a browser-host failure in the isolated harness. Verify input releases,
   an OSF UI Mod Issue appears, and the issue clears on confirmed recovery.
6. With no views demanded, verify no WebView helper starts. OSF Settings must
   remain usable when WebView2 is absent; requesting a WebView reports a failure.

The native test suite exercises this consumer against Slim's real SDK and UI's
real queued Views implementation. `tests/schema` validates both schemas with
Slim's actual parser. The frontend test drives the panel with real helper frames.
These checks do not replace the in-game acceptance steps above. Use the existing
OSF Test Harness policy and isolated profile when performing those checks.

The schema declares groups as a dictionary: `"groups": { "Panel": [...] }`.
The key supplies both the group ID and displayed label, and group declaration
order controls menu order. Hotkeys without a `group` use the first group, so
`openPanel` appears under **Panel**.
