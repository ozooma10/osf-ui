# Performance pass — October 22, 2026

Evidence is retained locally in `build/performance-pass-20261022/` (`E` below).
Game runs are in `build/backend-validation/game/`. No commit, tag or publication
was made during the original validation run. Initial tree was clean on `dev`, revision
`9d6d0c251a19b4f258823734936a27d6ab2a66a1`.

## Changes and priority

1. **Detached browser startup:** `BrowserHostClient.cpp` and
   `BrowserHostRuntime.cpp` resolve the browser storage directory through a
   Windows file handle once per process. In this desktop's inherited Codex
   filesystem context, logical LocalAppData actually maps into
   `Packages/OpenAI.Codex_2p2nqsd0c76g0/LocalCache/Local`. Explorer's detached
   browser did not share that redirection. Baseline launches repeatedly reached
   the 10-second broker timeout; a helper launched elsewhere could start but
   could not map the logical views directory. `E/physical-paths.json` proves
   the path mismatch. The change identifies the same existing directory and
   data for both processes; it does not relocate profiles or change the broker,
   privilege level, content URLs or public API. Ordinary paths stay equivalent;
   failed directory resolution retains the existing preparation failure path.
2. **Provider-density cache preparation:** `ViewCache::PrepareMods` now indexes
   paths it has just verified/published before its obsolete-entry scan. Normal
   entries avoid a quadratic sequence of filesystem identity queries. Unmatched
   entries still take the original `equivalent` fallback, preserving case-only
   renames, hard-link identity and stale-entry cleanup/retry behavior. This is
   occasional preparation on an existing worker, not per-frame game-thread work.
3. **Recovery correctness found during acceptance:** `BridgeApi` clears queued
   replies on the transition to unavailable, rather than every unavailable
   update. A replacement document can call a native endpoint before its load
   notification; previously its reply was discarded, producing a 10-second
   page timeout and a 30-second bridge timeout. Existing queue bounds and stale
   document token checks remain. A deterministic runtime test reproduces that
   ordering and fails against the unchanged implementation for both backends.

No engine work was moved to another thread. Presentation, renderer pacing,
serialization, public signatures, saved formats and cache fingerprints are unchanged.

## Matched cache measurements

The separate [cache benchmark](../../tools/profiling/view-cache/README.md) compiles
the real implementation. Each synthetic mod has HTML, a manifest and a 64 KiB
asset. Two baseline and two candidate processes ran in A-B-B-A order under the
shared harness lock, without builds, game sessions or browser benchmarks running.
Each row below reports the two medians of seven warm preparations, in milliseconds.
Baseline source is saved as `E/ViewCache.baseline.cpp`; runs are `cache-a1`,
`cache-b1`, `cache-b2`, `cache-a2`, with full CSVs and retained source fixtures.

| Mod folders | Baseline medians | Candidate medians | Interpretation |
| --- | ---: | ---: | --- |
| 1 | 2.55 / 2.52 | 2.54 / 2.61 | No demonstrated improvement |
| 8 | 13.21 / 13.27 | 11.19 / 11.12 | About 16% shorter preparation |
| 32 | 73.01 / 77.91 | 44.80 / 43.86 | About 41% shorter preparation |
| 128 | 628.08 / 623.54 | 160.61 / 160.52 | About 74% shorter preparation |

At 128 folders, baseline samples span 619.40–663.77 ms and candidate samples
159.83–169.84 ms. Process “other I/O” operations fall from 64,634 to 15,098.
Attribution-only builds on a separate identical 128-folder source tree show
8,256 identity calls consuming about 463–465 ms before, and zero for prepared
paths afterward. Those inclusive timings are nested in total preparation and
must not be added to it. Instrumented builds are separate from normal binaries.

These are ordinary NTFS fixtures, not live MO2 enumeration. OS caches were not
purged. The large-density gain is not a typical one-provider gain, an FPS gain,
or a measured reduction in total game/browser CPU. Initial copy and one-change
trials encountered intermittent publication access-denied errors in both
implementations. All failures remain in `failures.txt`; bounded fixture retries
are labeled `partial-retry`, never counted as cold-cache samples. No production
retry policy was changed and no cold-copy speedup is claimed.

An additional A-B-B-A control used a retained three-provider view tree (24 files,
1,289,953 bytes; snapshot and hashes in `E/retained-view-source*`). Warm medians
were 5.27/5.07 ms before and 4.73/4.59 ms after, roughly half a millisecond.
This small-content result limits the practical claim for ordinary provider counts.

## Rendering baseline and capture quality

`E/baseline-corrected` runs both production helpers at 1920×1080, 5-second warmup
and 15-second measurement per workload: idle, canvas, DOM, three views, and mixed
HUD workloads. Both editions passed all six pixel/marker checks and exited
cleanly. One repetition is triage, not a statistically supported optimization
comparison. The helpers were not optimized in this pass.

WebView2 CPU used 0.21% of one logical core while idle and approximately 13.74%,
20.00%, 36.77%, 24.26%, 23.11% for the five animated cases. Corresponding CEF
values were 1.77%, 16.98%, 26.35%, 51.13%, 35.61%, 38.21%. This aggregate process
work is not game-thread blocking. Unique helper output rates around 52–55 for
WebView2 and 60 for CEF are not Starfield FPS. Higher CEF browser work remains a
profiling lead; this capture does not attribute it sufficiently to justify a change.

`E/baseline-triage` is retained as a failed capture: mixed-HUD frames had valid
advancing markers but 821/821 invalid HUD pixel checks. At desktop DPR 1.25, the
fixture canvas lacked an explicit CSS size, scaling it twice. The one-line
fixture CSS correction is included; the corrected recapture uses it for both
backends. Production page assets were not changed.

## Validation and failed attempts

- Canonical XMake `releasedbg`, Windows x64, MSVC/VS 2026; CommonLibSF
  `2ff72a5096b481838780a4fde969176a622fc9b6`; nlohmann JSON 3.11.3;
  WebView2 SDK 1.0.4078.44 and observed runtime 155.0.4283.62; pinned CEF
  150.0.22 / Chromium 150.0.7871.256. Ryzen 9 5900X, RTX 3080,
  Windows 11 build 26300. Full configuration, dependency and binary inventories
  are in `E/starting-state.json`, `baseline-binaries.json` and benchmark metadata.
- Native cache, storage-path, API and both runtime lifecycle suites:
  **43/43 passed**, `E/final-native-regression.log`. Coverage includes stale
  cleanup, hard-link aliases, case-only rename, changed content, incomplete
  markers, locked-entry retry, recovery, cancellation and surface isolation.
  New storage tests check directory identity, Unicode children, repeated
  resolution and failure on a file used as a directory.
- Recovery test: **2/2 failed before**, specifically missing the early native
  reply (`recovery-before-detail.log`); both pass after the change. The initial
  production game candidate `20261022-235117-515-webview2-stock-background`
  reached actual HUD pixels, reload, native/Papyrus replies and observed
  save/load, then failed the forced-host-recovery echo. It is not counted as a pass.
- Canonical packaging compiles Papyrus and verifies ownership, exact ZIP bytes,
  selected backend and all **55 compiled Global Native signatures**. Normal
  build/deployment environment variables were unset inside build subprocesses;
  all test consumers and instrumentation are outside the production payload.
- Earlier baseline game runs `20261022-232400-143`, `232803-801` and `234410-638`
  (WebView2 stock) retain the broker timeouts. The discarded in-place executable
  experiment `234016-863` launched but failed view-directory mapping, confirming
  that changing executable launch location alone did not fix the filesystem boundary.
- A Task Scheduler experiment returned launch receipts without starting the
  cached helper; no receipt was counted as acceptance. Its production changes
  were discarded. A clean external runner (`234704-346`) was blocked by MO2's
  SFSE launch returning access denied; no security settings were changed.
- The first whole-directory storage lease exposed a restoration collision:
  `Move-Item` nested an intact backup into a recreated directory. All **254**
  original files were recovered with exact matching hashes (`234016-863`'s
  `storage-recovery.json`). The final harness resolves physical storage, leaves
  its root in place, waits for browser children, moves contents with collision
  checks, and verifies restoration by hash. Failed runs and private test data
  remain as evidence. Owned orphan brokers and the clean-run MO2 dialog were
  closed using verified process identities; temporary task registrations were removed.

## Final runtime, deployment and limits

Two fresh WebView2 production sessions passed:

| Run | Package | Result |
| --- | --- | --- |
| `20261022-235524-655-webview2-stock-background` | `perf-validated-webview2` | 70 assertions, 53 completed observations, 5 captures |
| `20261023-000034-454-webview2-stock-background` | `perf-deploy-webview2` | 70 assertions, 53 completed observations, 5 captures |

Both have zero failed assertions, no evidence warnings, clean helper exits,
restored browser/Settings storage and removed owned temporary MO2 directories.
The second run tests the exact deployed bytes after restoring the WebView2 build
configuration. Fresh post-recovery captures from both runs were visually checked:
gameplay, both corner markers, the animated HUD and successful native bridge text
are visible. Full outcomes, including failures, are in `E/runtime-summary.json`.
In the first passing run, the ordinary Explorer broker returned in about 173 ms
after HUD demand; the first shared frame was logged about 801 ms after demand.
Those log intervals are not measured presentation latency or rendered-frame times.

**CEF remains unaccepted for page reload.** Run
`20261022-235808-193-cef-stock-background` passed initial HUD pixel/native bridge,
state and event checks, then a new document timed out on its native `echo`
request and did not replay retained state. Its failure screenshot visibly shows
the timeout. Both CEF helper binary hashes match the saved baseline helpers,
but a matched full-game baseline could not run across the original storage-path
boundary; this pass does not establish whether the CEF reload defect is pre-existing.
CEF save/load and fault recovery were not reached. The CEF package was not deployed.

The verified production WebView2 package was deployed to
`C:\Modding\Starfield\MO2\mods\OSF UI` with the game/helpers stopped and shared
runner lock held. All 14 deployed files (including local PDB symbols) were hashed;
the package payload matches the accepted ZIP and the DLL/PDB match build output.
Installed ownership/backend/signature checks pass. Backup:
`E/deployment-backup`; receipts: `deployment-before.json`,
`deployment-receipt.json`, `deployed-package-check.log` and `cleanup-receipt.json`.

| Identity | SHA-256 |
| --- | --- |
| Initially installed DLL | `11D84A9FFA1779E360E535E0B13037F1AC9B1F829BD46721264ADE7E1CDFD454` |
| Fresh baseline DLL | `397D6F9D8DF61C9A289E61BD3EAA2BB5DF3C8B8656792969C030D3FAD5769A49` |
| Accepted/deployed DLL | `66B7C22CA4FB7DB39799A5D57165F6379FA20F854219AC8C0A005622F90FFFE2` |
| Accepted/deployed ZIP | `977FF12E364FB0FEAC04280E780021C178887224DE0E923BE2DC0D41BB57C694` |
| Accepted/deployed WebView2 helper | `1471D580CBDEC64BE3602A2993CEA68A9CCF9DE235DF1DC132B4583F4D38DCC3` |

The baseline is a fresh build of the starting source, not the initially installed
binary. Canonical packaging also synchronizes that source's core schema (removing
the installed obsolete World Screens group) and license files; optional extensions
were absent and were not installed. Stored Settings values were not edited.
Full source hashes, tracked patch and added source snapshots are retained under
`E/final-source-hashes.json`, `final-tracked.patch` and `added-source/`.

Cleanup verified all ten original Default profile file hashes unchanged, Default
selected, `releasedbg`/`webview2` restored, original deployment environment intact,
and no remaining game/browser/MO2 processes, temporary MO2 journals or owned
scheduled tasks. No instrumented binary or test consumer was deployed.

The local background suite uses the pinned disposable save, production OSF
Settings 1.2.0, Developer mode off, stock graphics and fresh isolated browser
storage. It observes HUD pixels, state/event delivery, reload, eight reopen
cycles, actual Papyrus replies, a newly written private save, a changed player
position restored by load, host termination/recovery and a 30-second soak.
Input acknowledgements alone are not treated as completion. The first post-load
capture was visually inspected: real gameplay and both browser corner markers
with a readable HUD and successful bridge status.

Beyond the explicit CEF failure, unmeasured or unaccepted: physical keyboard/mouse/gamepad, audio, Luma/Frame
Generation, 4K, real consumer workflows, remote web content, World Screens and
Browser TV (neither installed), long-session leak behavior, cold OS-cache timing,
allocations, lock-wait distributions, full CPU/GPU stack attribution and rendered
game-frame timing. No FPS improvement, general CPU reduction or regression-free
claim is supported. No unrelated mod, dependency or source profile is intentionally changed.
