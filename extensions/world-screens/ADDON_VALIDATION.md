# World Screens addon migration and validation

2026-10-19. Local development builds, uncommitted and unpublished. Starcade and
SSSE now consume the optional `OSFUIWorldScreens.dll` extension. Use the matching
unreleased core from this checkout; released OSF UI 2.1.0 lacks the surfaces ABI.

## Migrated content

- Starcade: native bindings and interaction use `OSFUI_WorldScreens.h`; ordinary
  state/events use `OSFUI.h`. The payload retains the 28 Red Mile placements,
  craftable TV, 29 game posters and shared play view. The Settings hotkey schema
  now uses the current object format. The portable launcher remains in the payload.
- SSSE: compiled display scripts use `OSFUIWorldScreens.BindScreen/UnbindScreen`.
  The payload includes the craftable DisplayScreen5 terminal, seven Key monitors,
  market overview and 64 company views. Stable company IDs and plugin local IDs
  are unchanged. The base exchange remains supplied by the base SSSE mod.
- Both builders use this extension's authoring tools. Catalogs moved to
  `SFSE/Plugins/OSF/WorldScreens/screens`; pages remain under `OSF/UI/views`.
  Neither addon bundles either framework DLL. Experimental worktree and deleted
  frontend CLI dependencies were removed from these build paths.

Game diagnosis also fixed three defects: decorative screen geometry now detaches
from the parent node instead of relying on a hidden flag; external mesh folder
names replace dots with underscores; and core input cancellation uses generations
so a loading/focus event while idle cannot reject the next activation. An A/B run
with the original SSSE model isolated the dotted external mesh path as the cause
of its blank face. Original model transforms, collision blocks and reference
placement are retained. The Key overrides drop their obsolete vanilla `XLMS`
material swap; that cleanup alone did not fix the blank face.

## Fresh game evidence

Successful run: [`20261019-232133-628-addons`](../../build/world-addon-migration/game/20261019-232133-628-addons/report.md),
19:21–19:27 EDT. Starfield 1.16.244.0, WebView2 155.0.4283.62, RTX 3080.
The shared harness used an owned process, a disposable save and a private MO2
profile with redirected Settings/Starcade state. The game exited normally.
No keyboard, mouse or controller input was injected.

| Check | Observed result |
| --- | --- |
| Split core/extension startup | Extension ready; 95 named views and 37 supported screen bases registered. |
| Seven Key monitors | Six distinct company pages (IDs 0–5) and the eight-row market overview received real Papyrus snapshots; captures show the web images on the physical monitors. |
| Save/load | Save file observed; load generation advanced from 1 to 2; all seven documents acquired new IDs; old native token 7 became invalid; displays rendered after load. |
| Red Mile | Multiple placed Starcade poster screens rendered without the old lettering overlay. |
| First play request | The real Starcade API admitted Orbital Paddle after loading/cell travel, with its game iframe and canvas visible on the physical screen. |
| Binding release | Releasing that reference produced a terminal interaction callback and released its binding. This deliberately tears down the display; it is not a test of normal page exit/idle-poster restoration. |
| Cell transitions | Key → Red Mile → QASmoke; logs record binding/resource retirement after engine/GPU ownership ends. |
| Craftable bases | Both real craftable ACTI bases were instantiated and bound (tokens 45 and 46). Capture provides partial visual evidence; no workshop UI placement was exercised. |

Useful captures: [Key after loading](../../build/world-addon-migration/game/20261019-232133-628-addons/key-after-load.png),
[Orbital Paddle](../../build/world-addon-migration/game/20261019-232133-628-addons/starcade-play-final.png),
[Red Mile](../../build/world-addon-migration/game/20261019-232133-628-addons/red-mile-final.png).
The run retains before/after snapshots, command observations, logs and captures.
Earlier failed diagnostic runs remain beside it; they do not count as acceptance.

## Build and offline checks

- Matching core, extension, Starcade native DLL and addon Papyrus builds passed.
- Core lifecycle suite: all 24 scenarios passed across WebView2/CEF doubles,
  including cancellation before activation and pending/active cancellation.
- Authoring helper: 2 Python tests passed, covering detached overlays, preserved
  model/collision data and safe external mesh paths.
- SSSE plugin: 5 Python tests passed, including reference field preservation and
  removal of the obsolete material swap. Board snapshot/selection: 6 Node tests passed.
- Core and extension package ownership checks passed. Core native export/Papyrus
  agreement covered 55 natives. Package checks do not establish game behavior.

## Keyboard and workshop follow-up

Completed on 2026-10-19 EDT (artifact names use UTC), using owned game processes,
private MO2 profiles and redirected saves. Keyboard/mouse input was injected through
the harness with explicit interactive authorization. The user waived physical
controller testing; no XInput controller or virtual gamepad was available.

- WebView2 workshop run:
  [`20261020-001531-574-addons-webview2`](../../build/world-addon-migration/game/20261020-001531-574-addons-webview2/report.md).
- WebView2 keyboard/focus evidence:
  [`20261019-233903-885-addons-webview2`](../../build/world-addon-migration/game/20261019-233903-885-addons-webview2/report.md).
  That run remains marked blocked because its workshop probe was added after VFS
  launch. Its completed keyboard/focus observations are retained; the later run
  closes the workshop gap.
- CEF run:
  [`20261020-010301-273-addons-cef`](../../build/world-addon-migration/game/20261020-010301-273-addons-cef/report.md),
  CEF 150.0.22 / Chromium 150.0.7871.256, accelerated OSR, sandbox enabled.

| Check | WebView2 | CEF |
| --- | --- | --- |
| Activate a placed Starcade TV and choose Play | Passed through the real activation menu. | Passed on the saved TV loaded from the WebView2 fixture. |
| Orbital Paddle keyboard input | ArrowUp moved the paddle; Enter reached the iframe and launched the ball. | ArrowUp moved the paddle to Y=59.5; trusted Enter down/up and a moving ball were observed. |
| Escape and control release | Idle poster restored; native release and subsequent player movement observed. | Idle poster restored; player position stayed fixed during play and moved 1.38 game units after Escape. |
| Focus loss and re-entry | Focus loss released interaction; a new play session was admitted. | Minimize released interaction; explicit focus restoration and re-entry succeeded. |
| Workshop build | Built both addon objects through Frontier Decorate; recipe materials were consumed and real pages rendered. | Deleted the imported objects, then built both addons afresh; materials were consumed and real pages rendered. |
| Workshop move/delete | Moved and deleted both screens through Modify mode. | Moved and deleted both imported screens through Modify mode; SSSE retained Nautilus after moving. |
| SSSE selection | Track company → Shipbuilding → Nautilus; company 52 received real price data. | The same menus selected Nautilus on the fresh display; company 52 received real price data. |
| Placement and selection persistence | Saved/reloaded both new objects; load generation 2→3, new document IDs, Nautilus retained. | Loaded the WebView2 placements in a fresh CEF process, then saved/reloaded newly built CEF objects; generation 2→3, new document IDs, Nautilus retained. |

The workshop fixture was a private copy of an existing ship save; the source save
was not modified. These are ship Decorate checks, not outpost construction tests.
Both successful sessions exited normally. No production code changed during this
follow-up; the changes are private test instrumentation and documentation.

Useful captures: [WebView2 gameplay](../../build/world-addon-migration/game/20261020-001531-574-addons-webview2/placed-starcade-playing.png),
[WebView2 reload](../../build/world-addon-migration/game/20261020-001531-574-addons-webview2/placed-screens-after-load.png),
[CEF gameplay](../../build/world-addon-migration/game/20261020-010301-273-addons-cef/cef-keyboard-playing.png),
[CEF SSSE move](../../build/world-addon-migration/game/20261020-010301-273-addons-cef/cef-ssse-moved.png),
[CEF reload](../../build/world-addon-migration/game/20261020-010301-273-addons-cef/cef-placed-after-load.png).

Placement limitation: the workshop accepts some positions on recessed/curved hab
walls where the world geometry clips the screen edges. This occurred with both
backends. Moving SSSE to a flat panel made its full face visible. Placement acceptance
does not mean every accepted wall position is visually clean.

Test infrastructure observations: CEF initially needed a minimize/explicit-focus
cycle before camera input worked; an automatic foreground-restoration attempt was
refused by Windows. A save-file assertion also needed its one-file result wrapped
in a collection. These attempts remain in the evidence; subsequent observed input
and save/load checks passed. The WebView2 Escape DOM wait raced retirement of the
play document, so release was established from the native log, restored poster and
player movement instead. These are not claimed as successful original waits.

The CEF Key visit returned fresh data for all seven feeds (six companies plus the
eight-row overview), but its captures did not establish physical-monitor visual
parity. The earlier WebView2 Key captures remain the visual evidence for that room.
The first CEF post-load snapshot was still connecting; the later retained report
has the same new document ID and populated Nautilus row. That evidence is reconciled
in [`cef-persistence-verified.json`](../../build/world-addon-migration/game/20261020-010301-273-addons-cef/cef-persistence-verified.json),
alongside the visible post-load capture.
The matching CEF core was built into a separate private staging directory:
[`editions/cef/manifest.json`](../../build/world-addon-migration/editions/cef/manifest.json).
The ordinary MO2 core deployment remains WebView2; its SHA-256 was rechecked and
the build configuration restored to WebView2. The final private workshop probe
compiled, both PowerShell scripts parsed, the observer passed Node syntax checking,
and the tracked diff passed whitespace checking.

## Local deployment

The final files were copied and SHA-256 checked after the game closed:

| MO2 mod folder | Files |
| --- | ---: |
| `OSF UI` (WebView2) | 13 |
| `OSF UI - World Screens` | 6 |
| `Starcade - World Screens` | 2,428 |
| `X2357 Settled Systems Stock Exchange` | 20 |
| `SSSE - World Screens` | 854 |

[`deployment.json`](../../build/world-addon-migration/deployment.json) records the
file hashes. A comparison against the successful private game payload found
**3,224 exact files plus 95 HTML files differing only by the appended private DOM
observer**, with no unexpected differences. See
[`tested-payload-comparison.json`](../../build/world-addon-migration/tested-payload-comparison.json).
The two SSSE addon script overrides account for the difference between summed
folder counts and 3,319 unique effective files. Test probes/observers are absent
from production payloads. Existing unrelated deployment files were preserved.

Enable core, extension and addon folders in that order, with SSSE's addon after
its base mod. Plugin dependencies are `x2357starcade.esm` →
`x2357starcade-world.esm`, and `x2357ssse.esm` → `x2357ssse-displays.esm` →
`x2357ssse-key-screens.esm`. Disable obsolete combined prototype mods that include
an old core DLL. Ordinary profile enablement was not changed. Existing local
archive files from before these fixes are not the validated deployment.

## Remaining acceptance

Physical controller buttons/sticks/Back remain untested by user choice. A complete
29-game gameplay sweep, game-specific exit paths beyond Escape, outpost placement,
multiple independently configured craftable displays, upgrade of an existing
world-screen-modded save, and a fresh core-only game run remain unverified. CEF Key
visual parity is also open. Native doubles do not replace these game checks. The
historical Starcade cabinet test harness still targets the earlier prototype and
was not used here. No commit, push, tag, upload or release was made.
