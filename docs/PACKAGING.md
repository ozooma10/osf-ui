# Packaging OSF UI for release

`tools/package.ps1` builds OSF UI and produces a mod-manager-installable archive under `dist/`. It is driven by the **same xmake install step that auto-deploys to MO2**, so the archive layout can never drift from what the game actually loads.

## Quick start

```powershell
# Full release build (Ultralight, releasedbg) -> dist/OSF-UI-v1.0.0-alpha.zip
# (version comes from kPluginVersion in src/core/Version.h; tag defaults to "alpha")
pwsh tools/package.ps1

# Custom version / tag
pwsh tools/package.ps1 -Version 1.0.0 -Tag beta

# Package the current build without rebuilding
pwsh tools/package.ps1 -SkipBuild

# Smaller archive without the 18 MB PDB (keeps crash logs less useful)
pwsh tools/package.ps1 -NoPdb
```

The Ultralight SDK must be available: the script reads `-UltralightSdkDir`, else `$env:ULTRALIGHT_SDK_DIR` else the gitignored `external/ultralight-free-sdk-1.4.0-win-x64` drop.

## What it does

1. **Configure + build** `releasedbg` with `--with_ultralight=true` (optimized, with a PDB). The real Ultralight renderer is required.
2. **`xmake install -o <staging>`** - lays down `SFSE/Plugins/OSFUI.dll` (+ PDB), and the SDK-sourced `ultralight/{bin,resources,license}` payload.
3. **Deterministic data sync** - the data folder (`config.json`, `vanillakeys.json`, `views/`, `settings/`) is copied straight from `data/OSFUI/`, and the Papyrus surface (`Scripts/OSFUI.pex` + `Scripts/Source/OSFUI.psc`) from `data/Scripts/`, over the staged tree — *not* trusted from xmake's install glob. xmake caches `add_installfiles("data/(OSFUI/**)")`; a view added or removed after the last clean reconfigure won't match disk. Syncing from source makes the archive exactly reflect `data/`.
4. **License docs** - `LICENSE`, `EXCEPTIONS`, `CREDITS.md` are placed inside `SFSE/Plugins/OSFUI/` (next to the Ultralight `ultralight/license/` folder), NOT at the archive root — the root maps onto the game's `Data\`, and loose doc files there would clutter every install. `LICENSE` + `EXCEPTIONS` are required: the GPL-3.0 §7 linking exception in `EXCEPTIONS` is what legally permits shipping the proprietary Ultralight binaries. `README.md` is not packaged (the Nexus/GitHub page serves that role).
5. **Verify** - fails loudly if the DLL, `config.json`, `vanillakeys.json`, the `osfui.json` settings schema, `OSFUI.pex`, any view manifest, or (for Ultralight builds) the Ultralight DLLs / EULA are missing. Also a **hard fail** if `config.json` references a view id (`<modId>/<viewName>`) with no matching `views/<modId>/<viewName>/manifest.json` in the archive — a standalone install must render out of the box.
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
      ├─ views/
      │  ├─ osfui/{settings,keybinds}/   (built-in views: views/<modId>/<viewName>/; + padnav.js asset)
      │  └─ shared/                      (shared view kit: osfui.css, osfui.js)
      ├─ settings/osfui.json          (OSF UI's own Mod Settings schema)
      └─ ultralight/{bin,resources,license}/
```

The archive root holds `SFSE/` and `Scripts/`, which map onto the game's `Data` folder - add the zip in a mod manager, or extract so they land in `<Starfield>/Data/`.

## What OSF UI does **not** package

- The Ultralight SDK headers/libs (build-time only, never redistributed beyond the runtime DLLs the license permits).
- Anything outside `data/` — in particular the dev/test surfaces: `devtools/harness/` (browser dev harness + mockbridge), `tests/` (native tests and the `tests/papyrus/` in-game Papyrus test mod, which deploys as its own separate MO2 mod), `examples/`, and `packaging/` (Nexus page assets). None of these can reach the archive: staging is the xmake install + the `data/` sync only.