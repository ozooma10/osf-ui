# World-surface validation — 2026-09-21

The live fixture rendered an animated WebView page on a physical QASmoke
DisplayScreen5. The final run passed 14 structured assertions and visual review,
including focus return, an independent passive HUD, and recovery after forcibly
stopping only the owned world browser helper.

## Retained game evidence

Artifacts are under `C:\Modding\Starfield\OSF Test Harness\artifacts\`.

| Run | Result |
| --- | --- |
| `20260921-142616-065-WorldSurface-Baseline` | Passed without OSFUI.dll; checkerboard visible on the screen and surrounding geometry rendered. |
| `20260921-143317-186-WorldSurface-Live` | Passed world-only animation and focus-loss/return checks. |
| `20260921-145307-306-WorldSurface-Live` | Passed combined screen/HUD rendering and forced world-browser recovery. |

Final command:

```powershell
pwsh -NoProfile -File tests/world-surface/Invoke-WorldSurfaceTest.ps1 -Mode Live -AimAtScreen -Overlay -RestartBrowser -KeepGame
pwsh -NoProfile -File tests/world-surface/Invoke-WorldSurfaceTest.ps1 -Action Stop
```

In the final run, completed GPU copies advanced from 26 to 57 and then 59 after
focus return. Material bindings remained at two descriptors. The deliberately
terminated browser was replaced; the ring generation advanced from 1 to 2,
cumulative completions reached 83, and the replacement produced 20 completed
frames. Diagnostics recorded one producer disconnect, no GPU failure, no ring
open failures, and no rejected frames. The original overlay helper survived.

Visual inspection of `live-screen-a.png`, `live-screen-b.png`,
and `live-screen-browser-recovered.png` confirmed
the animated card is on the physical screen, with readable orientation and the
surrounding world intact. After recovery the screen shows `FRAME 17` and
`BRIDGE READY`, while the independent HUD shows `TICK 78` and `BRIDGE READY`.
These labels exercise the real native message bridge in both documents.

The run retains the staged manifests/pages, plugin and DDS hashes, runtime
hashes, native observations, helper PID/start identities, current logs, and WGC
captures. Test-session cleanup restored all seven private profile files exactly;
the game and its helper processes exited and the ownership marker was removed.

Instrumented binaries used by the successful run (SHA256):

```text
OSFUI.dll
DCF7B634267ADF37604E2A37AE8516877C7E5EABC2B3A03DA4D4E42C1D84AA1F
osfui_webview2_host.exe
A9741847920C56EB36F51FC3C19D5A01E43FE343469B01ADFD4897B10207AB09
```

## Defects found through iteration

- Runs `20260921-144104-931` and `20260921-144538-364` exposed the passive HUD
  reveal timeout. Read-only client-size discovery incorrectly depended on
  installing input capture. Window discovery now works before input hooking.
- Run `20260921-144843-409` rendered both views but failed injected recovery:
  a dead producer's shared fence was mistaken for game-device loss. The backend
  now checks the game device and discards only the dead producer's ring. A failed
  remote consume signal still queues a local completion fence so copy allocators
  and resources remain safe to retire.
- Bridge recovery now clears the failed document's native event gate and
  deferred requests, then restores it for the replacement document's greeting.

Failed runs are retained with their original failed outcomes and cleanup proof.

## Source and build checks

| Check | Result |
| --- | --- |
| MSVC native manifest and presentation-controller suites | Both passed. |
| Native bridge API suite | 30 checks, 0 failures, including queued delivery through document recovery. |
| CLI world-view and type-surface tests | 5 tests passed. |
| Manifest schema Vitest file | 3 tests passed. |
| Frontend TypeScript | `tsc --noEmit` passed. |
| CLI public-asset synchronization | `--check` passed. |
| Fixture generator | Exact historical ESP and both DDS hashes verified. |

Commands used for these targeted checks:

```powershell
cmd /c .tmp\world-surface-native-check\run.cmd
cmd /c .tmp\world-surface-native-check\bridge.cmd
node --test frontend/packages/cli/test/world-view.test.mjs frontend/packages/cli/test/type-surface.test.mjs
npm --prefix frontend test -- test/build.manifest.test.ts
npm --prefix frontend run typecheck
node frontend/packages/cli/scripts/sync-public-assets.mjs --check
python tests/world-surface/make-fixture.py --out .tmp/world-surface-fixture-check
```

The local `.cmd` files retain the exact MSVC invocations against the real source
and the repository's native-test stubs. The normal build uses
`xmake f -P . -m releasedbg --test_harness=n -y` and
`xmake build -P . -j1 "OSF UI"`; the private test opener and snapshot logger are
excluded by that configuration.

The normal build completed and deployed successfully. Built and deployed DLL
and helper SHA256 hashes match; the normal DLL contains neither private test
marker. `test_harness` is false, and the production mod contains no screen-test
ESP or test view. Final `git diff --check --ignore-submodules=all` passed.

```text
Normal deployed OSFUI.dll
2C4D1614B0C50027E6CA1214EBE946A3CFAFD786A3EF76C707E909F0DE424216
Normal deployed osfui_webview2_host.exe
A9741847920C56EB36F51FC3C19D5A01E43FE343469B01ADFD4897B10207AB09
```

The fresh-game evidence above used the instrumented DLL. The final normal DLL
was rebuilt with the same rendering/recovery source and test instrumentation
disabled; its build and deployment checks are separate from that runtime proof.

## Scope of the original single-screen proof

This verifies one 1000x1000 world view on the preserved vanilla donor material,
with one simultaneous HUD, on this workstation. It does not establish four-view
stress performance, unrelated asynchronous compute sampling, interaction through
screen UVs, distance/cell suspension, or arbitrary authored material graphs.
The test-only Lodge texture overrides remain outside the production payload.
See [authoring requirements](../../docs/world-surfaces.md) and the
[repeatable test procedure](README.md).

## Four independent stock boards, 2026-09-21

Work was isolated on `osf/world-multiple-textures` at
`C:\Users\skunko\.codex\worktrees\world-multiple-textures\OSF UI`, based on
`f48c3bd`. Builds deployed into the worktree's `build/isolated-mods/OSF UI`,
and the runner copied those binaries to `OSF Testing - World Boards`.
The main checkout and ordinary MO2 runtime deployment were not edited.

The new fixture uses four vanilla DisplayScreen5 references with four original
material resource graphs and private textures. Placeholder dimensions are
1000, 1001, 1002, and 1003; all four browsers render at 1000x1000. The page comes
from `examples/world-stock-board` and receives different fictional prices via
the public native `SendToWeb(viewId, "stock.update", json)` API each second.

Artifacts under `C:\Modding\Starfield\OSF Test Harness\artifacts`:

- `20260921-223203-912-WorldSurface-Baseline`: asset-only pass with OSF UI absent;
  four differently tinted emissive screens and the surrounding world render.
- `20260921-223351-841-WorldSurface-Live`: 26 assertions passed, including all
  four bindings, progressing GPU copies, focus return, overlay coexistence,
  and one browser's forced termination/recovery.
- `20260921-223606-325-WorldSurface-Live`: 26 assertions passed again after
  increasing page padding to keep symbols clear of the vanilla screen border.
- `20260921-223842-570-WorldSurface-Live`: final 26-assertion pass with the
  camera shifted clear of the player. Inspected `multiple-world-boards.png`
  and `live-screen-browser-recovered.png`: NOVA, DEIM, STRO, and RYUJ all show
  distinct native prices/charts and `BRIDGE READY`. Only NOVA's update counter
  and chart restart after its browser is stopped; the others keep advancing.

The four world helpers have separate verified process identities. After
`world-0` terminates, only its browser/ring restarts; the other three process
identities, material descriptor counts, and ring generations remain unchanged,
their frames continue advancing, and the ordinary overlay remains alive.
Every run restored the private profile and stopped its owned game. Final cleanup
verified all seven profile backup hashes, no Starfield process, and no retained
world-surface ownership marker.

Instrumented DLL SHA256:
`925B8CBD0B9E4F8FB4EE64F140765BB46F73D62F17B4E102BF4788EF7BBC147A`.
Helper SHA256:
`26431140CA5A4B54F8A5C151B2FFBD74EBE32A2DB25A4A6BCA7837CDF1884986`.
Each run also retains hashes for its complete fixture and payload.

Build and host checks: MSVC releasedbg succeeded; all 30 native suites and all
22 CLI tests passed. The native policy tests cover cross-mod binding collisions
on both sides and disabled developer fixtures. The CLI tests cover four distinct
bindings at the same render resolution, duplicate sizes, and a fifth view.

This extends the earlier single-screen evidence to four concurrent, low-update
stock boards with independent native data and recovery. It does not establish
four simultaneous high-frame-rate games or 4K displays, nor remove the global
four-view cap, signature-based binding, or process-lifetime residency limit.
Production mod authors still own their meshes, materials, placement, and feed.

## Named texture identity and lifetime, 2026-09-21

This supersedes the dimension-signature and process-lifetime limitations of the
historical runs above. The worktree build passed two-namespace binding with
byte-identical placeholders, three engine unload/reload cycles, explicit DIRECT
retirement fences, output-budget deferral, and Starcade's Pong/animation regression.
See the [trace, artifact IDs, hashes and limits](../../docs/world-texture-identity.md)
and [repeatable commands](README.md#exact-asset-identity-and-lifetime).
The shared cached-worker runtime remains pending; this milestone proves the
asset identity and GPU lifetime foundation.
