# Packaging OSF UI for release

`tools/package.ps1` builds OSF UI and writes a mod-manager-installable archive to `dist/`. It stages through the **same xmake install step that auto-deploys to MO2**, so the archive can't drift from what the game loads.

## Quick start

```powershell
# Release build (WebView2, releasedbg) -> dist/OSF-UI-v<kPluginVersion>-alpha.zip
# Version comes from kPluginVersion in src/core/Version.h; tag defaults to "alpha".
pwsh tools/package.ps1

pwsh tools/package.ps1 -Version 1.4.0 -Tag beta   # custom version/tag
pwsh tools/package.ps1 -SkipBuild                 # package the current build
pwsh tools/package.ps1 -NoPdb                     # drop the 18 MB PDB (crash logs get less useful)
```

Needs the unpacked Microsoft.Web.WebView2 SDK: `-WebView2SdkDir`, else `$env:WEBVIEW2_SDK_DIR`, else `external/webview2`.

## Steps

0. `npm ci` from the committed lockfile.
1. Configure + build `releasedbg` with WebView2. The xmake hook generates built-in views from `frontend/src/` into ignored `build/frontend/views/`.
2. `xmake install -o <staging>` — stages views alongside `SFSE/Plugins/OSFUI.dll` (+ PDB) and `OSFUI/bin/osfui_webview2_host.exe`.
3. Deterministic data sync — copies authored data (`config.json`, `vanillakeys.json`, `settings/`) from `data/OSFUI/` and the Papyrus surface (`Scripts/OSFUI.pex`, `Scripts/Source/OSFUI.psc`) from `data/Scripts/` over the staged tree, preserving generated views and the host exe. Bypasses xmake's cached authored-data glob without source-controlling generated files.
4. License docs — `LICENSE`, `EXCEPTIONS`, `CREDITS.md` go inside `SFSE/Plugins/OSFUI/`, so installing doesn't clutter `Data\`.
5. Verify — hard-fails on a missing DLL, WebView2 host, `config.json`, `vanillakeys.json`, `osfui.json` schema, `OSFUI.pex`, any view manifest, the shared kit (`views/shared/osfui.js|.css`) or `views/osfui/padnav.js`, or a `config.json` view id with no manifest.
6. Sanity warnings (non-blocking) — flags `devMode` enabled in `config.json`.
7. Zip + report — `dist/OSF-UI-v<version>[-tag].zip`, with size and SHA-256.

## Archive layout (drop-in for MO2 / Vortex)

```
OSF-UI-v<version>-alpha.zip
├─ Scripts/
│  ├─ OSFUI.pex                      (Papyrus API surface)
│  └─ Source/OSFUI.psc               (source, for authors compiling against it)
└─ SFSE/Plugins/
   ├─ OSFUI.dll
   ├─ OSFUI.pdb                       (omit with -NoPdb)
   └─ OSFUI/
      ├─ LICENSE  EXCEPTIONS  CREDITS.md
      ├─ config.json
      ├─ vanillakeys.json             (vanilla-keybinds defaults table)
      ├─ views/                       (GENERATED from frontend/ during build)
      │  ├─ osfui/{settings,keybinds}/   (built-in views + padnav.js)
      │  └─ shared/                      (shared kit: osfui.css, osfui.js)
      ├─ settings/osfui.json          (OSF UI's own Mod Settings schema)
      └─ bin/osfui_webview2_host.exe
```

`SFSE/` and `Scripts/` map onto the game's `Data` folder — add the zip in a mod manager, or extract into `<Starfield>/Data/`.

## Not packaged

- The WebView2 SDK headers and static loader library (build-time only).
- Dev/test surfaces: only `build/frontend/views/` is installed; `frontend/` source, `node_modules`, the dev mock (`devmock/`, `osfui.mock.ts`), `tests/`, and `packaging/` are excluded.
- Source maps. The frontend build emits none and its output gate fails on a stray `.map`; nothing here excludes by extension, so one would otherwise ship.
