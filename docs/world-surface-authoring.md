# Authoring an in-world OSF UI screen

This is the verified Creation Kit workflow for displaying a live OSF UI web
view on a 3D screen. World surfaces remain experimental and compile only with
`xmake f --with_world_surfaces=y`. A screen can optionally open its assigned view
in OSF UI's normal fullscreen input surface when the player activates it. Direct
cursor interaction on the projected world texture remains a later milestone.

## What the author assigns

The binding is material-based, not tied directly to a placed reference FormID:

1. A `worldSurfaces` config entry assigns a web `view` to one canonical
   placeholder signature.
2. A screen material samples the matching placeholder DDS.
3. In Creation Kit, a Material Swap assigns that screen material to a placed
   reference.

Every loaded reference that resolves to the same placeholder displays the same
browser frame. To display a different view, use a different canonical
placeholder size and a separate material/swap. Per-reference view IDs are not
supported yet.

The runtime accepts a placeholder only when all of these match:

- its exact configured square, non-power-of-two dimensions;
- a typeless BGRA8 engine resource (`DXGI_FORMAT_B8G8R8A8_TYPELESS`);
- one mip, one array slice, one sample, and no render-target/depth/UAV flags.

The placeholder is never visibly sampled after binding succeeds. Its
checkerboard is the deliberate “not bound” diagnostic.

## 1. Generate a canonical placeholder

```bash
python tools/make_world_surface_placeholder.py --all
```

| Surface | Generated file | Binding dimensions |
| --- | --- | --- |
| 1 | `data/Textures/OSFUI/worldsurface_placeholder01.dds` | 1000×1000 |
| 2 | `data/Textures/OSFUI/worldsurface_placeholder02.dds` | 998×998 |
| 3 | `data/Textures/OSFUI/worldsurface_placeholder03.dds` | 1002×1002 |
| 4 | `data/Textures/OSFUI/worldsurface_placeholder04.dds` | 996×996 |

The visible pip row identifies the surface number. The four corner colors make
UV cropping, mirroring, and rotation obvious.

The DDS must ship byte-exact: uncompressed BGRA8, `DDSD_PITCH`, and no mip
chain. Do not run it through texconv or Creation Kit texture processing. A good
capture reports `resFormat 90`, `mips 1`, and an original `viewFormat 87` in
the SFSE log.

After copying/staging a donor texture, verify the actual destination file:

```bash
python tools/make_world_surface_placeholder.py --verify path/to/staged.dds
```

## 2. Configure the view

Add a research/dev override rather than changing the shipped config:

```json
{
  "worldSurfaces": [
    {
      "view": "yourmod.screens/terminal",
      "width": 1600,
      "height": 900,
      "placeholderWidth": 1000,
      "placeholderHeight": 1000,
      "activatePlugin": "OSFUI.esp",
      "activateFormId": "0x826",
      "activateEditorId": "OSFUI_WorldScreen_Settings"
    }
  ]
}
```

`view` must be a registered qualified view ID. Browser `width` and `height`
determine render resolution and aspect ratio; they do not need to match the
square placeholder. Placeholder width/height must match exactly one generated
DDS. For activation, `activatePlugin` plus `activateFormId` is the stable pair:
the filename is case-insensitive and the FormID is the plugin-local hexadecimal
ID of either the placed reference or its base Activator, not the session's
load-order-prefixed runtime ID. OSF UI resolves full, medium, and small plugin
tiers after data load. `activateEditorId` remains an optional compatibility
fallback, but Starfield usually does not retain EditorIDs on runtime forms, so
do not rely on it alone. Repeated view IDs, placeholder signatures, or activation
identities are first-wins; unsafe shapes and entries beyond the four-surface cap
are dropped with a warning while otherwise valid screens remain displayable.

## 3. Reproduce the verified Creation Kit proof

The currently verified baseline uses a vanilla `DisplayScreen5` Static and a
reference-level Material Swap. It proves the CK assignment model, material
presentation, and GPU transport without a custom mesh or custom `.mat`.

### Proof-only loose texture locations

Copy `worldsurface_placeholder01.dds` byte-for-byte to both paths below:

```text
Textures/Architecture/City/NewAtlantis/Lodge/NA_Lodge_Space01_color.DDS
Textures/Architecture/City/NewAtlantis/Lodge/NA_Lodge_Space01_emissive.DDS
```

This is a test harness, not a release asset: those are vanilla Lodge texture
paths. Shipping them would alter the Lodge wherever its material appears.

### CK steps

1. Launch Creation Kit through MO2 and load your test plugin as active.
2. In the Object Window, choose `World Objects > Static` and search for
   `DisplayScreen5`.
3. Drag a fresh `DisplayScreen5` into the test cell. Use a fresh reference so
   it cannot inherit an earlier experimental swap chain.
4. Select the new reference in the Render Window, right-click, and choose
   **Edit Material Swaps**.
5. Set exactly one row:

   ```text
   SetDressing\DisplayScreens\DisplayScreenWhite1.mat
     -> Architecture\City\NewAtlantis\Lodge\BaseMaterials\NA_Lodge_Space01.mat
   ```

6. Choose **Save Individual Swaps**. If CK reports that the swap already
   exists, accept it.
7. Choose **Apply**, save the active plugin, and close CK before launching the
   game.

Do not stack the older Opi material swap before this one. Material swaps apply
in sequence: once Opi replaces `DisplayScreenWhite1`, the later Lodge rule no
longer matches. The known-good reference contains one `XLMS` entry and only the
Lodge swap.

## 4. Optional activation-to-fullscreen interaction

The verified Static proves display only. To let the player press **E** on a
screen, create a bare Activator that uses the same model:

1. In Creation Kit's Object Window, open `World Objects > Activator`.
2. Right-click in the object list and choose **New**.
3. Give it a unique **ID**, for example `OSFUI_WorldScreen_Settings`, and a
   player-facing name. The ID is useful for authoring but is not the durable
   runtime binding.
4. Set its model to `SetDressing\DisplayScreens\DisplayScreen5.nif`.
5. Do not attach a terminal script or a vanilla terminal/Scaleform menu. The
   current prototype listens for the initial keyboard **E** edge in OSF UI's
   existing window-input hook and checks the player's crosshair target.
6. Place a fresh instance in the test cell and apply the same single
   `DisplayScreenWhite1` to `NA_Lodge_Space01` reference Material Swap used in
   the proof above.
7. Save the plugin. Inspect the Activator in xEdit and record its plugin-local
   FormID: remove the load-order prefix from the displayed FormID. The current
   `OSFUI.esp` proof Activator is local `0x826`.
8. Put the plugin filename and that local ID in `activatePlugin` and
   `activateFormId`, close CK/xEdit, and launch through the research build.

When the crosshair targets that Activator and the player initially presses keyboard **E**, OSF UI opens that entry's `view` through its established
fullscreen menu, focus, cursor, keyboard, and bridge path. The world screen
continues displaying the live view; the fullscreen surface handles input.

At startup the log records the plugin-local ID, its resolved runtime FormID and
plugin tier. An E press then records the targeted reference/base IDs and the view
that opens, or a precise no-target/no-match diagnostic.

This is intentionally not direct projected-screen input. That later path needs
a camera ray hit on the intended reference, triangle/barycentric conversion to
mesh UV coordinates, UV-to-browser pixel mapping, and explicit focus/cursor and
gameplay-input capture rules. None of those can be inferred from texture-binding
dimensions. Rebound Activate keys and gamepad activation are not wired into this
first keyboard proof.

## 5. Presentation findings

| Material | Result | Why |
| --- | --- | --- |
| `DisplayScreenWhite1.mat` | Frosted, washed-out bands | Uses the ship-window grunge roughness texture and high display emittance. |
| `OpiIntScreenUI01.mat` | Clearer but strongly reflective | No grunge, but constant roughness is approximately 0.1. |
| `NA_Lodge_Space01.mat` | Current readable baseline | Color-emissive, flat normal, no grunge map, and constant roughness 1.0. |

WebView2 publishes BGRA bytes in sRGB display space. OSF UI therefore creates
sRGB replacement views; sampling them as linear UNORM was the separate cause
of the earlier low-contrast, washed-out page. Material roughness controls the
remaining glare independently of that color-space correction.

The screen face should be 16:9 with full-rect 0..1 UVs for the default
1600×900 browser. A square placeholder does not imply square screen geometry.

## 6. Production asset contract

The proof above is intentionally not shippable because it overrides vanilla
Lodge textures. A release-ready screen needs all of the following:

- a material used only by the OSF screen;
- OSF-owned color and emissive texture paths containing the canonical
  placeholder bytes;
- matte presentation (flat normal, no glass/grunge roughness, high roughness);
- a CK Material Swap or custom mesh that assigns that isolated material only to
  intended references;
- an asset-only test with the OSF UI DLL removed, followed by runtime-only and
  combined tests.

A copied or hand-edited vanilla `.mat` is not acceptable. Starfield materials
embed `res:` content-database identities, and both copied materials and a
Material Editor Lite-authored replacement have previously made ordinary world
materials disappear. A new material is accepted only after the asset-only gate
passes twice.

A low-use vanilla test material may be useful as the next donor experiment,
but “one mesh user” is not sufficient for release. Its texture paths must also
be isolated, its presentation must be verified in-game, and replacing those
paths must have no player-visible blast radius. Until that passes, the Lodge
swap remains a proof harness and the normal release build must keep world
surfaces disabled.

## 7. Custom mesh route

A custom terminal is optional; the vanilla display proves it is not required
for the runtime. If a custom model is desired:

- author the geometry in Blender and export it with a Starfield-capable mesh
  toolchain;
- use a separate 16:9 screen face with full 0..1 UVs;
- assemble the `.nif` in a Starfield-capable NifSkope;
- assign only a material that has passed the production asset contract above;
- create a `Static` for display-only use, or a bare `Activator` with a unique
  plugin/local-FormID binding for activation-to-fullscreen interaction.

A malformed NIF or material can crash during `BSResourceNiBinaryStream` load or
break world rendering globally, so test material, mesh, plugin, and runtime as
separate variables before combining them.

## 8. Verify in game

A successful surface produces this sequence:

1. `[WorldSurface] material binding armed for N unique placeholder signature(s)`
2. `Runtime: world surface '<view>' ('world1') started at 1600x900`
3. `[WorldSurface] surface '<view>' adopted dedicated 1600x900 browser ring`
4. `[WorldSurface] surface '<view>' captured placeholder ... replaced=true`
5. `[WorldSurface] surface '<view>' consume signaling live`
6. continuing `descriptor refresh #N` messages

| Symptom | Meaning |
| --- | --- |
| Checkerboard, no capture | Material/texture did not load, or the placeholder was resized/recompressed. |
| Capture says `replaced=false` | Material arrived before the browser ring; it should self-heal once the ring is adopted. |
| Capture and ring, but checkerboard remains | View did not paint or the descriptor is being rebuilt; inspect runtime and host logs. |
| Black screen | Binding works, but the web view painted black or failed after load. |
| Missing/rearranged corner colors | Screen UVs are cropped, flipped, or rotated. |
| Many capture/collision warnings | Another loaded texture shares the signature; stop and choose another canonical slot. |
| Surrounding world disappears | Stop immediately and bisect asset-only versus runtime-only; never continue stacking material changes. |

## Release checklist

- [ ] Placeholder is generator-exact and loads as format 90 / one mip.
- [ ] CK reference has one intended material swap, not a chain.
- [ ] Asset-only run renders the surrounding world normally.
- [ ] Runtime-only run renders the surrounding world normally and captures nothing.
- [ ] Combined run shows the view and the surrounding world normally.
- [ ] The material and every overridden texture path are isolated from vanilla content.
- [ ] Screen remains readable in bright and dark cells and at oblique angles.
- [ ] If enabled, activation uses a bare Activator with a unique plugin/local-FormID binding and opens the intended view.
- [ ] Normal/release builds contain no research plugin, material, or placeholder assets.
