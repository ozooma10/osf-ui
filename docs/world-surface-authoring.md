# Authoring an in-world OSF UI screen (Creation Kit recipe)

This is the asset-side recipe for putting a live OSF UI web view on a 3D
screen in the world — a wall terminal, a desk monitor, a ship display. The
runtime side ships in the plugin (build with `xmake f --with_world_surfaces=y`);
this document covers everything the plugin cannot generate: the Creation
Kit-authored material, mesh, and placeable.

## The binding contract (read this first)

The runtime identifies your screen **by the exact pixel dimensions of the
placeholder texture** your material samples. When the engine creates a shader
resource view for a texture whose size matches a configured surface (and whose
shape passes the plain-sampled-texture guard), OSF UI rewrites that descriptor
to the surface's live browser texture, every tick, self-healing.

Consequences:

- The placeholder is **never visibly sampled** once the binding works. Its
  checkerboard pattern showing in-game is the designed "not working"
  indicator, and its corner colors + pip count are the diagnosis.
- The placeholder's dimensions must match the `placeholderWidth`/
  `placeholderHeight` of exactly one `worldSurfaces` config entry.
- Nothing else your mod (or any mod) loads may share those dimensions —
  that is why only the canonical generator sizes should be used.

## 1. Get a placeholder texture

```bash
python tools/make_world_surface_placeholder.py --all
```

| Surface slot | File | Size = config `placeholderWidth/Height` |
| --- | --- | --- |
| 1 | `data/Textures/OSFUI/worldsurface_placeholder01.dds` | 1000 |
| 2 | `data/Textures/OSFUI/worldsurface_placeholder02.dds` | 998 |
| 3 | `data/Textures/OSFUI/worldsurface_placeholder03.dds` | 1002 |
| 4 | `data/Textures/OSFUI/worldsurface_placeholder04.dds` | 996 |

The pip row in the pattern shows the slot number (N white squares = slot N).

**Hard rule: the DDS must ship byte-exact.** Uncompressed BGRA8, no mips,
`DDSD_PITCH` header — the generator asserts all of it. Never run the file
through texconv, CK texture import/recompression, or any pipeline that
"optimizes" textures. If you pack it into a `.ba2`, verify in-game that the
capture log still reports `resFormat 90, mips 1`; if it does not, ship the
texture loose.

## 2. One-time CK setup for material editing

Starfield has **no FO4-style TextureSet slot dialog**: materials reference
`.dds` files directly by path, so there is no TextureSet form to create. What
you need instead is to make the Material Editor writable. Create
`CreationKitCustom.ini` next to `CreationKit.exe` (it does not exist by
default) containing:

```ini
[Materials]
bUseCompiledDB=0
```

Without this the CK reads the compiled material database and the editor
cannot author new `.mat` files. Launch CK through MO2 (or place the
placeholder DDS in the real `Data\Textures\OSFUI\` folder) so the texture
path resolves.

## 3. Material — author fresh, never copy a vanilla .mat file

**Never file-copy or hand-edit a vanilla `.mat` to a new path** — including
the "edit the JSON in VS Code" workflow some community guides suggest.
Starfield materials embed `res:` content-database identities; a file-level
copy duplicates them, which corrupts the material graph and provably breaks
world rendering globally (see
[world-surface-investigation.md](world-surface-investigation.md), "Loose
material retargeting is not safe"). Creating or duplicating a material
**inside the Material Editor** is fine — the editor mints fresh identities;
it is copying at the file level that breaks.

In the Material Editor, create a new material under `Materials\OSFUI\` that:

- binds `Textures\OSFUI\worldsurface_placeholder01.dds` (or your slot's file)
  as **Albedo** and as **Emissive**;
- sets `EmissiveSettingsComponent.ExposureOffset = 6`.

The emissive binding is what makes the vanilla cockpit screens glow
(`ShipScreen_Avionics01_A.mat` precedent) — without it, browser content reads
as a dark decal instead of a lit display.

Alternative tooling: the standalone *Material Editor Lite* (Nexus mod 14659)
creates materials with an emissive-capable shader model without opening CK.
If you use it, verify the output the same way — a material that ships
duplicated `res:` IDs fails exactly like a file copy. Its built-in texture
conversion must NOT be pointed at the placeholder (step-1 hard rule).

## 4. Mesh

The screen face of your mesh must:

- be **16:9 geometry** — the browser renders 1600x900 by default, and the
  quad's proportions are what keep it uncropped and unstretched. (The
  placeholder being square is irrelevant: it is never visibly sampled.)
- map **full-rect 0..1 UVs** onto that face. Any atlas packing or UV crop
  shows up as missing corner colors in the placeholder pattern — each corner
  is a distinct color precisely so a crop tells you which edge is lost.
- expect DirectX-style V orientation. If the image appears upside down or
  mirrored, the corner colors identify the flip (red = top-left,
  green = top-right, blue = bottom-left, yellow = bottom-right).

A flat quad inset into any terminal/monitor prop works; the screen face just
needs its own material slot pointing at the step-3 material.

Toolchain reality: mesh geometry is authored in Blender and exported to
Starfield's `.mesh` format (community "Geo Bridge" exporter), the `.nif`
wrapping it is assembled in a Starfield-capable NifSkope, and **the material
is assigned in NifSkope** — the screen face's `BSGeometry` node carries the
material path string; point it at your `Materials\OSFUI\...mat`.

## 5. Placeable form + plugin

Create a **Static** form (display-only in v1 — pick Activator only when the
future interaction milestone lands) referencing the mesh, editor ID prefixed
`OSFUI_` (e.g. `OSFUI_WorldScreen01`), in a small dedicated `.esm`.

File layout in the repo (everything below `data/` deploys to the mod root):

```
data/
  OSFUI_WorldScreens.esm
  Materials/OSFUI/worldscreen01.mat
  Textures/OSFUI/worldsurface_placeholder01.dds   (generated at build time)
```

## 6. Config wiring

In `data/OSFUI/config.json` (dev/testing; the shipped config omits this key):

```json
"worldSurfaces": [
  {
    "view": "yourmod.screens/terminal",
    "width": 1600,
    "height": 900,
    "placeholderWidth": 1000,
    "placeholderHeight": 1000
  }
]
```

`view` must be a registered view id (`data/OSFUI/views/<mod>/<view>/`);
`placeholderWidth`/`Height` must equal the dimensions of the DDS your material
references. The validator drops entries that are screen-shaped, power-of-two,
non-square, duplicated, or beyond the 4-surface cap — each drop is a WARN in
the SFSE log naming the reason.

## 7. Verify in-game

Place the form (`help OSFUI_WorldScreen01`, `player.placeatme <formid>`) and
walk up to it. A good session logs, per surface, in order:

1. `[WorldSurface] material binding armed for N unique placeholder signature(s)`
2. `Runtime: world surface '<view>' ('world1') started at 1600x900` and a
   `OSF UI.webview2-host.world1.log` file appears
3. `[WorldSurface] surface '<view>' adopted dedicated 1600x900 browser ring`
4. `[WorldSurface] surface '<view>' captured placeholder ... replaced=true`
   (on approaching the placed screen)
5. `[WorldSurface] surface '<view>' consume signaling live` and
   `descriptor refresh #N` lines with growing N

### Failure modes

| Symptom | Meaning |
| --- | --- |
| Checkerboard visible, no `captured placeholder` line | The material never loaded, or its texture was recompressed/resized (check `resFormat 90, mips 1` in any capture line) |
| Checkerboard, `captured` logged but no `adopted ... ring` | The surface's browser host never came up — read `OSF UI.webview2-host.world1.log` |
| Checkerboard, `adopted` + `captured` but no `consume signaling live` | The view never painted — wrong view id, or a load failure logged by `Runtime: world surface ... failed to load` |
| Wrong pip count on the checkerboard | The mesh's material references a different slot's placeholder than the config entry drives |
| Corner colors missing / rearranged | Mesh UV crop / flip — see step 4 |
| `N captures ... signature is probably colliding` WARN | Another texture shares the placeholder size — stop, switch to a different canonical size |
| Screen black (not checkerboard) | Binding works; the page painted black — debug the view like any web view (`devMode`, F12) |

Only after the full sequence passes with the CK-authored asset does the
`with_world_surfaces` build flag graduate to on-by-default.
