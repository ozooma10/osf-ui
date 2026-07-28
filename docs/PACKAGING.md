# Packaging OSF UI for release

`tools/package.ps1` builds OSF UI and produces a mod-manager-installable archive under `dist/`. It is driven by the **same xmake install step that auto-deploys to MO2**, so the archive layout can never drift from what the game actually loads.

## Quick start

```powershell
# Full release build (WebView2, releasedbg) -> dist/OSF-UI-v<kPluginVersion>-alpha.zip
# (version comes from kPluginVersion in src/core/Version.h; tag defaults to "alpha")
pwsh tools/package.ps1

# Custom version / tag
pwsh tools/package.ps1 -Version 1.4.0 -Tag beta

# Package the current build without rebuilding
pwsh tools/package.ps1 -SkipBuild

# Smaller archive without the 18 MB PDB (keeps crash logs less useful)
pwsh tools/package.ps1 -NoPdb

```

The unpacked Microsoft.Web.WebView2 SDK package must be available: the script reads `-WebView2SdkDir`, else `$env:WEBVIEW2_SDK_DIR`, else `external/webview2`.

## What it does

0. **Install frontend dependencies** with `npm ci` from the committed lockfile.
1. **Configure + build** `releasedbg` with `--with_webview2=true` (optimized, with a PDB). The xmake hook generates built-in views from `frontend/src/` into ignored `build/frontend/views/`.
2. **`xmake install -o <staging>`** - rebuilds and stages the views alongside `SFSE/Plugins/OSFUI.dll` (+ PDB) and `OSFUI/bin/osfui_webview2_host.exe`.
3. **Deterministic data sync** - authored data (`config.json`, `vanillakeys.json`, `settings/`) is copied straight from `data/OSFUI/`, and the Papyrus surface (`Scripts/OSFUI.pex` + `Scripts/Source/OSFUI.psc`) from `data/Scripts/`, over the staged tree while preserving generated views and the host executable. This bypasses xmake's cached authored-data glob without making generated files source-controlled.
4. **License docs** - `LICENSE`, `EXCEPTIONS`, and `CREDITS.md` are placed inside `SFSE/Plugins/OSFUI/`, not at the archive root, so installing the archive does not clutter the game's `Data\` directory.
5. **Verify** - fails loudly if the DLL, WebView2 host, `config.json`, `vanillakeys.json`, the `osfui.json` settings schema, `OSFUI.pex`, or any view manifest is missing. The shared kit (`views/shared/osfui.js`, `views/shared/osfui.css`) and `views/osfui/padnav.js` are required too. It also hard-fails if `config.json` references a view id with no matching manifest.
6. **Sanity warnings** (non-blocking) - flags a `config.json` with `devMode` enabled.
7. **Zip + report** - writes `dist/OSF-UI-v<version>[-tag].zip` and prints its size and SHA-256.

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
      ├─ LICENSE  EXCEPTIONS  CREDITS.md   (license docs; kept inside the plugin folder so Data root stays clean)
      ├─ config.json
      ├─ vanillakeys.json             (vanilla-keybinds defaults table)
      ├─ views/                          (GENERATED from frontend/ during build)
      │  ├─ osfui/{settings,keybinds}/   (built-in views: views/<modId>/<viewName>/; + padnav.js asset)
      │  └─ shared/                      (shared view kit: osfui.css, osfui.js)
      ├─ settings/osfui.json          (OSF UI's own Mod Settings schema)
      └─ bin/osfui_webview2_host.exe
```

The archive root holds `SFSE/` and `Scripts/`, which map onto the game's `Data` folder - add the zip in a mod manager, or extract so they land in `<Starfield>/Data/`.

## What OSF UI does **not** package

- The Microsoft.Web.WebView2 SDK headers and static loader library (build-time only).
- Development/test surfaces stay out of the archive: only `build/frontend/views/` from `frontend/` is installed, while its source, `node_modules`, harness, `tests/`, `examples/`, and `packaging/` are excluded. Staging is driven by xmake install plus the authored `data/` sync.
- Source maps. The frontend build emits none, and its output gate fails on a stray `.map` — nothing in this script or CI excludes by extension, so one would otherwise ship in every archive.
