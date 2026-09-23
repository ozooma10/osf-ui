# Settings example

A development-only plugin that opens a view from an OSF Settings hotkey and forwards one setting to the page. Not built or packaged with OSF UI.

- `osfui-example.json` declares `showDetails` and the **Open example panel** hotkey (F6; rebind in Settings).
- At `kPostPostLoad` the plugin acquires both SDKs, subscribes before reading `showDetails`, and publishes only that boolean with `SetViewState`.
- The hotkey callback queues `RequestMenu` and returns; it never calls the engine.
- The panel joins the Launcher tab as **OSF UI Example**, has a text field and slider for input testing, and closes with its button or Escape.

## Build and stage

From the repository root with submodules and MSVC/XMake:

```powershell
$env:XSE_SF_MODS_PATH = $null   # keep the example out of the normal deploy
$env:XSE_SF_GAME_PATH = $null
xmake f -P examples/settings-view -y -m releasedbg
xmake build -P examples/settings-view -y
xmake install -P examples/settings-view -o out/settings-view-example -y "OSF UI Settings Example"
```

Install the staged output as its own MO2 mod next to OSF UI and OSF Settings, enable it, start Starfield, and press F6. The example uses the production package's shared `osfui.js`; the schema must be present before launch.

## Acceptance

1. With **Show details** off, F6 opens the panel without details.
2. Close, enable the setting in OSF Settings, reopen: details appear.
3. Rebind the hotkey, restart, use the new binding.
4. Gameplay hotkeys are blocked while the panel captures input and work again after closing.
5. Cause a browser-host failure: input releases, an OSF UI Mod Issue appears, and it clears on recovery.
6. With no view requested, no WebView helper starts. OSF Settings stays usable without WebView2; requesting a view reports a failure.

`tests/native` exercises this plugin against the vendored Settings SDK and UI's queued request path. It does not replace the in-game steps.
