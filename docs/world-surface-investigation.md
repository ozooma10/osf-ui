# Web UI on world-space screens: investigation notes

Status: end-to-end world-material rendering is proven on Starfield 1.16.244.
WebView content reached a vanilla cockpit mesh through a targeted D3D12 SRV in
a dev-only proof.

## What the engine already has

Starfield contains a concrete
`CreationRendererPrivate::ScaleformToTextureRenderPass`. This is not only an
RTTI name: the UI render-graph builder selects that pass when a Scaleform render
item has both a movie object and an attached render-target object. The same
builder selects the adjacent normal `ScaleformRenderPass` when the attached
target is absent.

The relevant Address Library identities on 1.16.244 are:

| Symbol | Address Library ID | RVA |
| --- | ---: | ---: |
| `UIRenderPass` vtable | 497421 | `0x04EE2E58` |
| `ScaleformToTextureRenderPass` vtable | 497640 | `0x04EE4980` |
| `ScaleformRenderPass` vtable | 497642 | `0x04EE49C8` |
| UI render-graph builder | 145957 | `0x02B6C660` |
| Scaleform pass execute | 146151 | `0x02B91730` |

Static disassembly of the builder shows the to-texture object receiving:

- a render-item handle at offset `+0xF0`;
- color binding `0x069D7D00` at `+0xF4`;
- depth binding `0x069D7E01` at `+0xF8`.

This establishes that an engine-supported offscreen UI path exists. It does not
yet establish whether its output is a persistent texture that a world material
can consume, or a transient render-graph resource used only by another UI pass.
That distinction determines whether OSF UI can reuse the native path directly.

## Instrumentation

`ScaleformToTextureProbe` is an observational, dev-mode-only vtable probe for
the pass execute function. It validates the exact vtable and implementation IDs
above, fails closed if another game build or hook is present, forwards the
original function unchanged, and logs a bounded sample of:

- render thread ID and pass/context pointers;
- the render-item, color-binding, and depth-binding values;
- CPU time spent in the original pass.

The probe is dormant while `devMode` is false. Search the SFSE log for
`[ScaleformToTextureProbe]`.

## Live capture procedure

1. Set `devMode` to `true` in the deployed `Data/OSFUI/config.json`.
2. Launch through MO2/SFSE and confirm the log contains
   `[ScaleformToTextureProbe] armed`.
3. Visit these cases, noting the time and location for each:
   - title and pause menus;
   - inventory/data menu;
   - ship builder and photo mode;
   - a usable world terminal;
   - a cockpit or hab display;
   - any known video or animated world screen.
4. Search the log for probe `call=` entries. Record which interaction begins or
   changes the call stream and whether `renderItem` remains stable.
5. If a reproducible trigger exists, capture one frame with RenderDoc and locate
   the pass using the logged trigger. Record the output resource's dimensions,
   format, lifetime, state transitions, and later shader-resource consumers.

No calls means the tested content did not instantiate this native pass. Calls
without a later world-material consumer indicate an internal UI-only use. A
stable output that is subsequently sampled by a mesh material is the strong
signal that the native route can be adapted.


The first live run produced no calls while testing cockpit displays and
full-screen terminals. Cockpit screens instead resolve through ordinary world
materials. Static asset tracing identified this concrete probe target:

| Asset | Value |
| --- | --- |
| Mesh | `meshes/ships/interior/cockpitscreens/shipscreen_avionics01/shipscreen_avionics01.nif` |
| Material | `Materials/Ships/Interior/CockpitScreens/ShipScreen_Avionics01_A.mat` |
| Color texture | `textures/ships/interior/cockpitscreens/shipscreen_avionics01_color.dds` |
| Texture archive | `Starfield - Textures10.ba2`, record 2221 |
| Runtime signature | 1024x1024, 11 mips, BC1 sRGB |

`WorldTextureProbe` now observes `ID3D12Device::CreateShaderResourceView` only
in dev mode. It matches the temporary uniquely sized cockpit texture and can
replace that one descriptor with a WebView shared-ring resource.

A temporary 1000x1000 loose DDS made the target
material/resource unambiguous during the proof:

| Proof datum | Observed value |
| --- | --- |
| Candidate resource | 1000x1000, one mip, resource format 90 |
| Engine descriptor | SRV view format 87 (`B8G8R8A8_UNORM`) |
| Loader call | Address Library ID 142989, call site RVA `0x02A112D5` |
| Replacement | WebView shared-ring slot 0 opened on Starfield's D3D12 device |

The checkerboard first proved that the loose DDS selected the central avionics
material. The subsequent run rewrote only that material's SRV to sample WebView
ring slot 0. After the overlay advanced the shared ring, the browser content
appeared on the cockpit mesh. This proves the complete
descriptor-to-world-geometry path.

The first browser image was black because slot 0 contained a frame published
before the handoff view finished loading. Opening OSF UI advanced consumption
and repainted slot 0; this is a ring-lifecycle limitation, not a binding failure.

## Dedicated-ring implementation: first live run and the two-host collision

`WorldSurface` now hooks `ID3D12Device::CreateShaderResourceView` (vtable slot
18), captures the descriptor of any texture matching the configured placeholder
dimensions, and rewrites it to the current completed slot of a dedicated
renderer/ring that runs independently of the fullscreen overlay.

The first live run reached two of the three milestones and stalled on the
third:

| Milestone | Result |
| --- | --- |
| `[WorldSurface] material binding armed` | logged at first engine device discovery |
| `[WorldSurface] captured placeholder 1000x1000` | logged on cockpit entry |
| `[WorldSurface] adopted dedicated ... browser ring` | never logged |

The binding hook was fine; the dedicated renderer's host process never came
up. Two independent launch collisions, both keyed "one per game process":

1. **Pipe-name collision.** Each renderer worker seeds its pipe-name nonce
   from `GetTickCount64() ^ (pid << 17)`. The overlay and world-surface
   workers start in the same timer tick, so both generated the identical pipe
   name; the second `CreateNamedPipeW` (first-instance flag) failed with
   `ERROR_PIPE_BUSY` (231).
2. **Single-instance lock.** The host exe held
   `Local\osfui-wv2-host-<gamePid>`, so the second host process exited
   immediately with "another host instance is already running for this game
   pid".

The fix gives each renderer instance (`RendererConfig::instanceName`, `"world"`
for the world surface) its own pipe nonce/name, host-side instance lock
(`Local\osfui-wv2-host-<pid>-<instance>`), WebView2 user-data folder, views
mirror, and host log file, and passes `--instance=<tag>` to the host. The
renderer-side focus watchdog also now compares the focused window's pid
against its own host's pid only — otherwise the world-surface renderer would
have revoked focus from the overlay host's browser child during interactive
menus.

Expected log on a good run: `armed` → `captured placeholder` →
`adopted dedicated 1600x900 browser ring` → `placeholder descriptor now
samples browser ring slot N`. Bounded warnings fire if the produce fence
stops completing instead.

## Next engineering steps

1. Give world surfaces a dedicated view and shared ring instead of borrowing the overlay ring.
2. Track the current fully produced slot and signal its consume fence while the surface is visible.
3. Refresh the targeted descriptor safely when the current slot changes or the ring is recreated.
4. Match browser resolution/aspect, UV crop, color space, alpha, and emissive treatment to the mesh.
5. Add per-instance lifecycle, visibility throttling, and raycast-to-UV input mapping.

A custom screen mesh/material with an OSF UI-owned placeholder texture is the
safer production target. It gives stable identity and dimensions without
overriding a vanilla cockpit asset.

## Feasibility assessment

The feature is proven feasible end to end: OSF UI browser pixels were sampled
by a Starfield world mesh without CPU readback. Remaining work is
productionization: dedicated surface rings, slot/fence lifecycle, stable custom
material identity, correct presentation, UV hit-testing, and safe cell/device
lifetime handling.

A static or periodically refreshed display is a moderate prototype. A fully
interactive terminal with focus, cursor mapping, persistence, multiple
simultaneous screens, and robust cell/device lifecycle handling is a larger
feature rather than a small compositor extension.
