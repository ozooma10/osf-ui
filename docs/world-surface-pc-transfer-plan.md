# In-world Web UI: PC transfer and continuation plan

Status captured: 2026-07-29, after the first successful world-screen render and
activation-to-fullscreen proof.

## 1. Known-good checkpoint

### Source

- Repository: `https://github.com/ozooma10/osf-ui.git`
- Branch: `main`
- Current pushed commit: `c3b84abc768d16880c2caeed116eeef025e5802d`
- World-screen implementation commit:
  `fa6abd0` (`feat: stabilize in-world web screens`)
- CommonLibSF submodule:
  `e15f73fad583ddaca9d361dd073d5a0b138dd10b`
- The source checkout is configured for a **normal build**
  (`with_world_surfaces=false`). The DLL deployed in MO2 is a separately saved
  research build. A plain `xmake build` on a new checkout will not contain the
  world-surface runtime until the research option is enabled.

The following working-tree directories are not tracked by Git and must either
be committed separately or copied manually before the old PC is retired:

```text
examples/skeleton-hud/
examples/vanilla-native/
```

### Game and tools

- Starfield: `1.16.244.0`
- Creation Kit: `1.16.244.0`
- Current game directory:
  `C:\Program Files (x86)\Steam\steamapps\common\Starfield`
- Current MO2 directory: `C:\Modding\Starfield\MO2`
- Selected MO2 profile: `Default`
- `profile_local_saves=true`; the proof save is therefore under the profile
  and must be copied with it.
- xmake: `3.0.8+master.a97128837`
- Node: `24.16.0` (the repository requires Node `>=20.19`)
- npm: `11.16.0`
- Python: `3.14.5`
- WebView2 SDK used by setup: `Microsoft.Web.WebView2 1.0.4078.44`
- Build deployment environment:
  `XSE_SF_MODS_PATH=C:\Modding\Starfield\MO2\mods`

### Proven behavior

The known-good test currently does all of the following:

1. A `DisplayScreen5` reference renders the live `osfui/settings` browser view.
2. The surrounding Starfield world renders normally.
3. The matte Lodge donor gives readable output without the former severe
   reflection or washed-out color.
4. Aiming at the Activator and pressing physical keyboard **E** opens the same
   view through OSF UI's normal fullscreen interactive surface.
5. Activation resolves `OSFUI.esp` local FormID `0x826` through the loaded-file
   table, rather than depending on a load-order-prefixed runtime ID or a
   usually-missing EditorID.

This is a successful feasibility proof, not a release asset. It still
overrides two vanilla Lodge texture paths.

### Current proof construction

- Plugin: `OSFUI.esp`
- Activator base local FormID: `0x826`
- Placed reference local FormID: `0x825`
- Activator EditorID: `OSFUI_WorldScreen_Settings`
- Model: `SetDressing\DisplayScreens\DisplayScreen5.nif`
- Reference Material Swap:

  ```text
  SetDressing\DisplayScreens\DisplayScreenWhite1.mat
    -> Architecture\City\NewAtlantis\Lodge\BaseMaterials\NA_Lodge_Space01.mat
  ```

- Both Lodge color and emissive texture paths currently contain the canonical
  slot-1, 1000x1000 placeholder.
- Browser output is 1600x900.
- Input is physical keyboard E only. Rebound Activate keys, gamepad activation,
  and direct pointer input on the projected texture are not implemented.

The active override config is:

```json
{
  "configVersion": 1,
  "enabled": true,
  "view": "osfui/settings",
  "views": ["osfui/settings", "osfui/keybinds"],
  "devMode": false,
  "worldSurfaces": [
    {
      "view": "osfui/settings",
      "width": 1600,
      "height": 900,
      "placeholderWidth": 1000,
      "placeholderHeight": 1000,
      "activateEditorId": "OSFUI_WorldScreen_Settings",
      "activatePlugin": "OSFUI.esp",
      "activateFormId": "0x826"
    }
  ]
}
```

## 2. Copy from the old PC

Close Starfield, Creation Kit, xEdit, and MO2 before copying.

### Required

The safest option is to copy the entire
`C:\Modding\Starfield\MO2` directory. If that is impractical, preserve at
least:

```text
C:\Modding\Starfield\MO2\ModOrganizer.ini
C:\Modding\Starfield\MO2\profiles\Default\
C:\Modding\Starfield\MO2\mods\OSF UI\
C:\Modding\Starfield\MO2\mods\Address Library for SFSE Plugins\
C:\Modding\Starfield\MO2\overwrite\OSFUI.esp
C:\Modding\Starfield\MO2\overwrite\SFSE\Plugins\OSFUI\config.json
```

Copying `profiles\Default` is important: it preserves `modlist.txt`,
`plugins.txt`, `loadorder.txt`, and the profile-local save containing the test
reference. In that profile, `OSF UI` and Address Library are enabled and
`OSFUI.esp` is enabled.

Also preserve these known-good recovery points:

```text
C:\tmp\osfui-activation-formid-research-20260729\
C:\tmp\osfui-display-screen-proof-passed-20260729\
C:\tmp\osfui-pre-activation-deploy-20260729-0100\
```

The first directory contains the exact research DLL that passed activation.
The second is the clean visual proof. The third is a useful pre-activation
fallback. Other `C:\tmp\osfui-*` directories are experiment archaeology and
are optional.

Copy these Creation Kit preferences if the new install should retain the same
editor setup:

```text
C:\Program Files (x86)\Steam\steamapps\common\Starfield\CreationKit.ini
C:\Program Files (x86)\Steam\steamapps\common\Starfield\CreationKitPrefs.ini
```

Optionally preserve the successful session logs from:

```text
C:\Users\<user>\Documents\My Games\Starfield\SFSE\Logs\
```

### Checksums for the known-good active state

| File | SHA-256 |
| --- | --- |
| `mods\OSF UI\SFSE\Plugins\OSFUI.dll` | `C947244E4061FAFA9ADF56D292E885665057482351EAD8AE5A04DAFB94D42706` |
| `mods\OSF UI\SFSE\Plugins\OSFUI\bin\osfui_webview2_host.exe` | `FEAC166F1BC00CEACFD085BE5945E4F528FB4A2084A74BDBA6A429FAD1558D84` |
| `overwrite\OSFUI.esp` | `3A58EF2A2D619BD2060BB134C36D480180FFF9FE6D0E03006A0E482A0F815789` |
| `overwrite\SFSE\Plugins\OSFUI\config.json` | `7D725F0952EB37B5426058ABE9BDF4E3F2F94C759C170A9824C5AFDFB81F041E` |
| `mods\OSF UI\...\NA_Lodge_Space01_color.DDS` | `817905FB906C5EE63BA30511DC985B092CA133D91455CAE5D49A4FB1CF9EDD21` |
| `mods\OSF UI\...\NA_Lodge_Space01_emissive.DDS` | `817905FB906C5EE63BA30511DC985B092CA133D91455CAE5D49A4FB1CF9EDD21` |
| Address Library `versionlib-1-16-244-0.bin` | `299EA1B4DA35B42E9BF1B8ED94FA980694A4DCBEBFC7693201619C4C08FA49D8` |

The full proof texture paths are:

```text
mods\OSF UI\Textures\Architecture\City\NewAtlantis\Lodge\
  NA_Lodge_Space01_color.DDS
  NA_Lodge_Space01_emissive.DDS
```

Verify a restored file with:

```powershell
Get-FileHash -Algorithm SHA256 -LiteralPath "C:\path\to\file"
```

## 3. Prepare the new PC

Install:

1. Starfield and Creation Kit. Reproduce the proof on `1.16.244.0` first.
   If the game has updated, stop and obtain matching SFSE and Address Library
   before loading the research DLL. Do not assume the 1.16.244 runtime layouts
   remain valid.
2. MO2 and the `Default` profile copied above.
3. SFSE matching the installed Starfield version.
4. Address Library containing the matching `versionlib` file.
5. Microsoft Edge WebView2 Evergreen Runtime.
6. Visual Studio with Desktop C++ and a Windows SDK.
7. Git, xmake 3.0+, Node 20.19+, npm, and Python 3.
8. Creation Kit, xEdit/SF1Edit, and the Starfield mesh/material tools needed
   for future asset work.

If the directory layout changes, set `XSE_SF_MODS_PATH` to the new MO2 `mods`
directory. The value points to `mods`, not directly to the `OSF UI` child:

```powershell
$env:XSE_SF_MODS_PATH = "C:\Modding\Starfield\MO2\mods"
```

Persist it in the user environment after the location is final.

## 4. Restore and build the source

```powershell
git clone --recurse-submodules https://github.com/ozooma10/osf-ui.git `
  "C:\Modding\Starfield\OSF UI"
Set-Location "C:\Modding\Starfield\OSF UI"
git checkout c3b84abc768d16880c2caeed116eeef025e5802d
git submodule update --init --recursive
npm ci
npm --prefix frontend ci
pwsh tools/setup.ps1
xmake f -m release --with_world_surfaces=y
xmake build
```

`xmake build` auto-deploys the DLL, built-in views, host, and generated
placeholder textures when `XSE_SF_MODS_PATH` is set. It does **not** recreate
the proof plugin, override config, profile save, or the two manually copied
Lodge-path DDS files. Restore those from the transfer after building.

Do not run a normal `xmake f ... --with_world_surfaces=n` build over the test
deployment until the known-good proof has been reproduced: normal deployment
intentionally removes research assets. The current source checkout was left
normal-gated even though the active MO2 DLL was the saved research binary.

## 5. Reproduce the baseline before changing anything

1. Select MO2 profile `Default`.
2. Confirm `OSF UI` and Address Library are enabled.
3. Confirm `OSFUI.esp` is enabled.
4. Confirm the override config contains the plugin/FormID binding above.
5. Launch with SFSE through MO2 and load the transferred proof save.
6. Confirm the surrounding world renders normally.
7. Confirm the display shows the OSF UI settings page rather than a
   checkerboard or vanilla screen.
8. Aim at the Activator and press physical keyboard E once.
9. Confirm the fullscreen settings view opens and accepts input.
10. Exit normally and archive the new session log as the new-PC baseline.

Expected log evidence includes:

- world-surface material binding armed for the 1000x1000 signature;
- the browser ring adopted at 1600x900;
- placeholder capture with `replaced=true`;
- `OSFUI.esp` local FormID `0x826` resolved to a runtime FormID;
- one keyboard-E activation binding armed;
- the target activation opening `osfui/settings`.

If the baseline fails, do not begin asset authoring. Compare the checksums and
use this diagnosis order:

| Symptom | First check |
| --- | --- |
| Startup `Invalid ID: 0` | Wrong/old DLL; restore the known-good DLL or rebuild `fa6abd0` or later. |
| Checkerboard remains | Missing capture; verify both proof DDS files and the 1000x1000 config signature. |
| Vanilla/default screen | Lodge-path proof DDS files were not restored or lost MO2 conflict priority. |
| World becomes invisible | Stop immediately; remove newly introduced material/NIF files and return to the visual-proof backup. |
| E does nothing | Confirm `OSFUI.esp` is enabled and config binds `OSFUI.esp` + `0x826`; inspect the no-target/no-match log. |
| Browser host fails | Verify WebView2 runtime and the deployed host checksum/path. |

## 6. Engineering continuation plan

Create a branch only after the transferred baseline is green:

```powershell
git switch -c osf/world-screen-assets
```

### Milestone A — durability pass

Run the existing proof through:

- a fresh process launch;
- repeated E activation and fullscreen close/reopen;
- save/reload;
- leave and re-enter the cell;
- fast travel away and back;
- enable another harmless plugin before `OSFUI.esp` to prove the
  plugin-local FormID binding survives a changed runtime load index;
- bright and dark lighting and oblique viewing angles.

Exit criteria:

- the world never disappears;
- the browser recovers after cell/save transitions;
- activation always opens the intended view;
- no target-collision or repeated-capture warnings appear.

### Milestone B — isolated release-safe asset

This is the critical path. Replace the proof-only Lodge override with:

- an OSF-owned material;
- OSF-owned color/emissive texture paths;
- a matte presentation (high roughness, flat normal, no glass/grunge layer);
- a screen face with full 0..1 UVs and 16:9 geometry;
- a CK Material Swap or custom mesh affecting only the intended reference.

Use strict bisection for every candidate:

1. Known-good baseline.
2. Asset/plugin only, with the OSF UI DLL disabled.
3. Runtime only, with the candidate asset/plugin disabled.
4. Combined asset and runtime.
5. Remove the candidate and repeat once to prove any failure follows it.

Reject the candidate immediately if it makes ordinary world materials
disappear, crashes during `BSResourceNiBinaryStream` loading, affects a vanilla
location, or requires a shared vanilla texture override.

Never reintroduce the previously bad files:

```text
Materials\OSFUI\OSFUI_WorldScreen01.mat
Meshes\OSFUI\WorldScreens\OSFUIWorldScreen01.nif
```

Exit criteria:

- neither Lodge proof DDS is installed;
- asset-only, runtime-only, and combined gates all pass twice;
- the display is readable in representative lighting;
- the asset has no vanilla material/texture blast radius.

### Milestone C — mod-author contract

Keep Creation Kit responsible for the physical object:

- Activator or reference;
- model and material;
- placement and activation prompt.

Keep the web assignment in a mod-owned OSF UI manifest rather than requiring
Creation Kit to understand web view IDs. The target shape should be equivalent
to:

```json
{
  "worldSurfaces": [
    {
      "id": "settings-screen",
      "view": "yourmod/screen",
      "placeholderSlot": 1,
      "width": 1600,
      "height": 900,
      "activation": {
        "plugin": "YourMod.esm",
        "localFormId": "0x1234"
      }
    }
  ]
}
```

Implement per-mod discovery and validation, retain plugin/local FormID as the
durable identity, and treat EditorID only as diagnostic metadata. Provide one
small reference plugin and one manifest as the authoring example.

Exit criteria:

- a second mod can assign its own registered view without editing OSF UI's
  global developer config;
- duplicate placeholder and activation bindings fail safely with useful logs;
- the example works with a changed load order.

### Milestone D — production activation input

Replace the physical-E prototype with the engine's mapped Activate action:

- respect keyboard rebinding;
- support controller/gamepad Activate;
- trigger only on the initial edge;
- do not double-open if Starfield also activates the bare object;
- preserve normal gameplay input when the crosshair is not on an OSF screen.

Direct pointer interaction on the world texture is explicitly not part of this
milestone.

### Milestone E — lifecycle and performance

- Detect when a screen's cell/reference is not relevant.
- Suspend or stop browser hosts for unloaded/invisible screens.
- Recover shared rings and descriptors after re-entry.
- Keep the existing small surface cap and report resource usage/failures in
  System Health.

### Milestone F — optional direct ray-to-UV interaction

Only pursue this if the product needs interaction without opening fullscreen.
It requires:

1. camera/cursor raycast to the intended reference;
2. triangle hit and barycentric coordinates;
3. mesh UV interpolation;
4. UV-to-browser pixel mapping with orientation handling;
5. world-space cursor/focus rules;
6. explicit gameplay input capture and escape behavior.

This is substantially larger than activation-to-fullscreen and should not
block a first shippable world-screen release.

## 7. Release gate

World surfaces must remain behind `with_world_surfaces` until:

- the isolated asset passes every bisection gate;
- a clean MO2 profile can install it without vanilla overrides;
- normal builds contain no world-surface code markers or research assets;
- research builds survive the durability matrix;
- mod-author configuration and failure logging are documented;
- keyboard rebinding/gamepad behavior is either implemented or clearly scoped
  as an experimental limitation.

The first action on the new PC is therefore not new development: reproduce the
exact known-good render-and-E-activation baseline. The first development task
after that is Milestone B, the isolated release-safe screen asset.
