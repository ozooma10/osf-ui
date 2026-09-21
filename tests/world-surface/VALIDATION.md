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

## Scope of proof

This verifies one 1000x1000 world view on the preserved vanilla donor material,
with one simultaneous HUD, on this workstation. It does not establish four-view
stress performance, unrelated asynchronous compute sampling, interaction through
screen UVs, distance/cell suspension, or arbitrary authored material graphs.
The test-only Lodge texture overrides remain outside the production payload.
See [authoring requirements](../../docs/world-surfaces.md) and the
[repeatable test procedure](README.md).
