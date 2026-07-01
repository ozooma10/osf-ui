# Packaging OSF UI for release

`tools/package.ps1` builds OSF UI and produces a mod-manager-installable archive
under `dist/`. It is driven by the **same xmake install step that auto-deploys to
MO2**, so the archive layout can never drift from what the game actually loads.

## Quick start

```powershell
# Full release build (Ultralight, releasedbg) -> dist/OSF-UI-v0.1.0-alpha.zip
pwsh tools/package.ps1

# Custom version / tag
pwsh tools/package.ps1 -Version 0.1.0 -Tag beta

# Package the current build without rebuilding
pwsh tools/package.ps1 -SkipBuild

# Smaller archive without the 18 MB PDB (keeps crash logs less useful)
pwsh tools/package.ps1 -NoPdb
```

The Ultralight SDK must be available (proprietary, never vendored): the script
reads `-UltralightSdkDir`, else `$env:ULTRALIGHT_SDK_DIR`, else the gitignored
`external/ultralight-free-sdk-1.4.0-win-x64` drop.

## What it does

1. **Configure + build** `releasedbg` with `--with_ultralight=true` (optimized,
   with a PDB). The real Ultralight renderer is required — the shipped
   `config.json` selects `renderer: "ultralight"`, and a null-renderer build
   would silently fall back and render nothing.
2. **`xmake install -o <staging>`** — lays down `SFSE/Plugins/OSFUI.dll`
   (+ PDB), and the SDK-sourced `ultralight/{bin,resources,license}` payload.
3. **Deterministic data sync** — the data folder (`config.json`, `views/`,
   `settings/`) is copied straight from `data/OSFUI/` over the staged tree,
   *not* trusted from xmake's install glob. xmake caches
   `add_installfiles("data/(OSFUI/**)")`; a view added or removed after the last
   clean reconfigure won't match disk. Syncing from source makes the archive
   exactly reflect `data/OSFUI/`. (Only the SDK `ultralight/` folder is
   preserved from the install step.)
4. **Root docs** — `LICENSE`, `EXCEPTIONS`, `README.md`, `CREDITS.md` are placed
   at the archive root. `LICENSE` + `EXCEPTIONS` are load-bearing: the GPL-3.0
   §7 linking exception in `EXCEPTIONS` is what legally permits shipping the
   proprietary Ultralight binaries; the Ultralight EULA/NOTICES ship under
   `ultralight/license/`.
5. **Verify** — fails loudly if the DLL, `config.json`, any view, or (for
   Ultralight builds) the Ultralight DLLs / EULA are missing.
6. **Sanity warnings** (non-blocking) — flags a `config.json` that points at the
   `osf` view (needs the separate OSF Animation mod), or has `focusMenu` /
   `disableControls` / `devMode` enabled.
7. **Zip + report** — writes `dist/OSF-UI-v<version>[-tag].zip` and prints its
   size and SHA-256.

## Archive layout (drop-in for MO2 / Vortex)

```
OSF-UI-v0.1.0-alpha.zip
├─ LICENSE  EXCEPTIONS  README.md  CREDITS.md
└─ SFSE/Plugins/
   ├─ OSFUI.dll
   ├─ OSFUI.pdb                       (omit with -NoPdb)
   └─ OSFUI/
      ├─ config.json
      ├─ views/{hud,settings,test}/   (built-in demo views)
      ├─ settings/*.json
      └─ ultralight/{bin,resources,license}/
```

The archive root holds `SFSE/`, which maps onto the game's `Data` folder — add
the zip in a mod manager, or extract so `SFSE/` lands in `<Starfield>/Data/`.

## What OSF UI does **not** package

- **The `osf` scene-browser view + `osf.*` handlers belong to OSF Animation**,
  which ships them in its own mod (its `views/osf/` is the authoritative copy).
  OSF UI is the framework; it packages only its built-in demo views. For the
  flagship experience a user installs both mods, then points `config.json`'s
  `view`/`views` at `osf`.
- The Ultralight SDK headers/libs (build-time only, never redistributed beyond
  the runtime DLLs the license permits).

## Player-facing requirements (put on the mod page)

- Starfield **1.16.244** (Steam) + **SFSE** + **Address Library**.
- Not compatible with Xbox / Game Pass (SFSE is Steam-only).
- HDR / DLSS-G frame-gen / ReShade coexistence is untested — warn alpha testers.
