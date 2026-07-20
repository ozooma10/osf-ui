# Packaging OSF UI for release

`tools/package.ps1` builds OSF UI and produces a mod-manager-installable archive under `dist/`. It is driven by the **same xmake install step that auto-deploys to MO2**, so the archive layout can never drift from what the game actually loads.

## Quick start

```powershell
# Full release build (WebView2, releasedbg) -> dist/OSF-UI-v1.0.0-alpha.zip
# (version comes from kPluginVersion in src/core/Version.h; tag defaults to "alpha")
pwsh tools/package.ps1

# Custom version / tag
pwsh tools/package.ps1 -Version 1.0.0 -Tag beta

# Package the current build without rebuilding
pwsh tools/package.ps1 -SkipBuild

# Smaller archive without the 18 MB PDB (keeps crash logs less useful)
pwsh tools/package.ps1 -NoPdb

# Regenerate the views from frontend/ and hard-fail if the committed output was
# stale -- what a real release build should use (needs npm on PATH)
pwsh tools/package.ps1 -RebuildFrontend
```

The unpacked Microsoft.Web.WebView2 SDK package must be available: the script reads `-WebView2SdkDir`, else `$env:WEBVIEW2_SDK_DIR`, else `external/webview2`.

## What it does

0. **Generated-view freshness check** - `data/OSFUI/views/` is build output of `frontend/` (Vite + TypeScript + Preact), committed to git so this script, `xmake install` and the MO2 redeploy can all consume it *without Node*. Shipping a stale bundle is invisible until someone opens the overlay in game, so it is checked here. The default is deliberately Node-free and advisory: if the generated tree is dirty in git, you get a warning telling you to rebuild. `-RebuildFrontend` runs `npm ci` + `npm run build` + `npm run check:dist` instead and **hard-fails on drift** — use it for anything you actually publish. CI enforces the same invariant on every push (the `frontend` job's "Generated view output is up to date" step), so a stale commit cannot merge.
1. **Configure + build** `releasedbg` with `--with_webview2=true` (optimized, with a PDB).
2. **`xmake install -o <staging>`** - lays down `SFSE/Plugins/OSFUI.dll` (+ PDB) and `OSFUI/bin/osfui_webview2_host.exe`.
3. **Deterministic data sync** - the data folder (`config.json`, `vanillakeys.json`, `views/`, `settings/`) is copied straight from `data/OSFUI/`, and the Papyrus surface (`Scripts/OSFUI.pex` + `Scripts/Source/OSFUI.psc`) from `data/Scripts/`, over the staged tree — *not* trusted from xmake's install glob. xmake caches `add_installfiles("data/(OSFUI/**)")`; a view added or removed after the last clean reconfigure won't match disk. Syncing from source makes the archive exactly reflect `data/`.
4. **License docs** - `LICENSE`, `EXCEPTIONS`, and `CREDITS.md` are placed inside `SFSE/Plugins/OSFUI/`, not at the archive root, so installing the archive does not clutter the game's `Data\` directory.
5. **Verify** - fails loudly if the DLL, WebView2 host, `config.json`, `vanillakeys.json`, the `osfui.json` settings schema, `OSFUI.pex`, or any view manifest is missing. The shared kit (`views/shared/osfui.js`, `views/shared/osfui.css`) and `views/osfui/padnav.js` are required too. It also hard-fails if `config.json` references a view id with no matching manifest.
6. **Sanity warnings** (non-blocking) - flags a `config.json` with `devMode` enabled.
7. **Zip + report** - writes `dist/OSF-UI-v<version>[-tag].zip` and prints its size and SHA-256.

## Archive layout (drop-in for MO2 / Vortex)

```
OSF-UI-v1.0.0-alpha.zip
├─ Scripts/
│  ├─ OSFUI.pex                      (Papyrus API surface)
│  └─ Source/OSFUI.psc               (source, for authors compiling against it)
└─ SFSE/Plugins/
   ├─ OSFUI.dll
   ├─ OSFUI.pdb                       (omit with -NoPdb)
   └─ OSFUI/
      ├─ LICENSE  EXCEPTIONS  CREDITS.md   (license docs; kept inside the plugin folder so Data root stays clean)
      ├─ config.json
      ├─ vanillakeys.json             (vanilla-keybinds defaults table)
      ├─ views/                          (GENERATED from frontend/, committed; never hand-edited)
      │  ├─ osfui/{settings,keybinds}/   (built-in views: views/<modId>/<viewName>/; + padnav.js asset)
      │  └─ shared/                      (shared view kit: osfui.css, osfui.js)
      ├─ settings/osfui.json          (OSF UI's own Mod Settings schema)
      └─ bin/osfui_webview2_host.exe
```

The archive root holds `SFSE/` and `Scripts/`, which map onto the game's `Data` folder - add the zip in a mod manager, or extract so they land in `<Starfield>/Data/`.

## What OSF UI does **not** package

- The Microsoft.Web.WebView2 SDK headers and static loader library (build-time only).
- Anything outside `data/` — in particular the dev/test surfaces: `frontend/` (the view *source*, its `node_modules`, and the dev harness — only its build output under `data/OSFUI/views/` ships), `devtools/harness/` (the older browser dev harness + mockbridge), `tests/` (native tests and the `tests/papyrus/` in-game Papyrus test mod, which deploys as its own separate MO2 mod), `examples/`, and `packaging/` (Nexus page assets). None of these can reach the archive: staging is the xmake install + the `data/` sync only.
- Source maps. The frontend build emits none, and its output gate fails on a stray `.map` — nothing in this script or CI excludes by extension, so one would otherwise ship in every archive.
