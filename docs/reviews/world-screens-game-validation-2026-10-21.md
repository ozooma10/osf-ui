# World-screen game validation, 2026-10-21

Both WebView2 and CEF have passing accepted cases for multiple TVs, save/load,
navigation failure/stop, video mute/unmute, sustained audio, cell unload/return,
and terminated/stalled host recovery. Live testing found and fixed a navigation
error-retention defect. The original setup is restored and verified.

The clearest measured performance benefit is native game-drain responsiveness
during a stalled host's retirement: baseline snapshots became about 6.2 seconds
old; current snapshots stayed within 102 ms on WebView2 and 107 ms on CEF.
Steady CPU results are mixed. There is no demonstrated overall CPU or FPS gain.

## Revisions and accepted runs

Before is `49d1a0afcffab1b2ea5cb0d77f106d6bb724bba9`. After runtime code is
`ccee7b6e0fe3694c30c21b0bc44e6cfcdf92349a`. Both include the same pre-existing,
uncommitted WebView2 IPC changes. They are held constant, not attributed to this
review. The initial provenance file describes the earlier candidate; the final
`payloads/after/*/receipt.json` files identify the rebuilt navigation-fix candidate.

All paths below are relative to `build/world-screen-validation/` unless stated.

| Backend | Baseline run | Current run |
| --- | --- | --- |
| WebView2 | `20261021-195049-643-baseline-webview2` | `20261021-194104-364-current-webview2` |
| CEF | `20261021-192539-135-baseline-cef` | `20261021-191421-407-current-cef` |

Full payload hashes are in `payloads/{before,after}/{webview2,cef}/receipt.json`.
The performance manifests verify actual loaded core, extension, and host binaries
against their private payloads. Current core SHA-256 values are:

- WebView2: `35E143A2A327AA085DB2DCDA9F035CE9035B465A196058D4864F04A0BE6157D8`.
- CEF: `913205F8AF363CE81F3AB792509A23E32B69C89DFBF6AAD07A950048851A3DEE`.

Both edition probes and all 55 compiled Papyrus signatures passed. Both final
core builds and the CEF host build passed with live deployment disabled. The
navigation regression passed 12 checks; browser-surface lifecycle scenarios
passed 103 checks in each backend build.

## Functional results

| Scenario | WebView2 | CEF | Evidence checked |
| --- | --- | --- | --- |
| Multiple TVs | Pass | Pass | Two distinct bindings share playback; UI reports two screens; emitter sequence 2 -> 1 -> 0 -> 2, with measured output changes |
| Save/load | Pass | Pass | Private checkpoint, new document and binding identities, two restored TVs and measured audio |
| Navigation | Pass | Pass | Observed pending load stops; missing-domain failure settles with an error; return home works |
| Video and mute | Pass | Pass | Remote WebM, mute/unmute, measured output; WGC images visually show the same movie on both TVs |
| Sustained audio | Pass | Pass after readiness fix | 30-second tone, healthy native routes and endpoint measurements |
| Cell unload/return | Pass | Pass | Old helper retires; private checkpoint reload restores two TVs and audio |
| Terminated helper | Pass | Pass | Exact owned helper exits; new process/document and measured audio |
| Stalled helper | Pass | Pass | Heartbeat recovery retires the suspended process, creates a replacement, and restores measured audio |

Current WebView2's run-level result is passed. Current CEF deliberately retains a
failed run-level result because its first sustained-audio attempt began before
the restarted stream reported active. The fixture now waits for readiness;
`case-sustained-audio-ready.json` passed. The original failed case remains intact.
All other accepted CEF cases listed above passed.

Each accepted 30-second tone capture recorded 1,440,480 stereo endpoint frames at
48 kHz, with zero discontinuities, timestamp errors, or invalid samples. CEF's
native PCM generation remained stable and its dropped-packet delta was zero.
The controlled 997 Hz component measured RMS 0.010511 on WebView2 and 0.022158 on
CEF. These are separate backend runs, not a loudness-equivalence test. Endpoint
measurement includes all desktop audio; route state, the controlled frequency,
and enable/mute controls provide the supporting source evidence.

The synthetic tone is explicitly restarted after save/load and host recreation.
These checks establish recovery, not gap-free playback across browser destruction.
Terminated-host document/audio readiness took 2,227/2,700 ms on WebView2 and
2,153/3,701 ms on CEF. The audio timings precede the separate endpoint measurement.

## Steady performance

Machine: Ryzen 9 7950X (16 cores, 32 logical processors), RTX 3080 driver
32.0.16.1742, Starfield 1.16.244.0. Each run uses the same pinned cell, camera,
position, two 1920x1080 TV surfaces, and private settings. Captured game output is
2560x1409; the captured window is 2562x1441. Display/quality configuration matches,
including enabled dynamic resolution. Developer diagnostics are enabled in both
variants. Current runs precede their baselines; order was not randomized.

After 120 seconds of warmup, each workload has three attempted 30-second captures
at 1 Hz. Admission and analysis check owned process identities, loaded hashes,
stable process/document sets, foreground state, unpaused loaded gameplay, cell,
position, camera pose, browser state, and actual rendering dimensions. Active
builds and foreign game/helper processes are excluded. An idle reusable MSBuild
worker is permitted only after verifying its CPU is not advancing.

There are 24 final captures, 23 individually valid captures, and 11 matched pairs.
WebView2 tone repeat 13 is excluded because the baseline browser process set
changed. Its current counterpart is consequently unpaired. All other workloads
have three matched repeats. No failed capture was silently replaced in these
final run directories.

CPU values below are medians of per-capture time-weighted CPU usage; 100% is one
logical core. Total browser includes the host and its Chromium process tree.
Ranges show observed repeat variation, not confidence intervals.

| Backend / workload | Pairs | Browser CPU before [range] | Browser CPU after [range] | Relative change |
| --- | ---: | ---: | ---: | ---: |
| WebView2, two-TV tone | 2 | 7.351% [6.867, 7.835] | 5.404% [5.213, 5.596] | -26.5% |
| WebView2, home/tone off | 3 | 5.855% [5.648, 7.357] | 6.649% [6.569, 6.809] | +13.5% |
| CEF, two-TV tone | 3 | 4.474% [4.260, 5.632] | 5.069% [4.629, 5.433] | +13.3% |
| CEF, home/tone off | 3 | 2.717% [1.815, 3.729] | 2.611% [1.918, 2.990] | -3.9% |

WebView2 tone CPU is lower in both accepted pairs, but the sample is small and
comes from one process launch per variant. The other CPU ranges overlap and
paired directions are mixed. These observations do not establish a general CPU
speedup or attribute a change to one optimization.

| Backend / workload | Host CPU before -> after | Whole-game CPU before -> after | Browser private MiB before -> after |
| --- | ---: | ---: | ---: |
| WebView2, two-TV tone | 0.772 -> 0.666 | 325.921 -> 319.441 | 380.992 -> 397.123 (+4.2%) |
| WebView2, home/tone off | 0.693 -> 0.691 | 328.299 -> 327.693 | 383.948 -> 399.572 (+4.1%) |
| CEF, two-TV tone | 1.065 -> 1.174 | 311.947 -> 318.948 | 352.055 -> 359.943 (+2.2%) |
| CEF, home/tone off | 0.479 -> 0.533 | 315.378 -> 322.603 | 352.273 -> 363.142 (+3.1%) |

Browser private memory increased in every paired capture; this is not a memory
reduction. Short captures do not establish a leak or explain the allocation
change. Whole-game CPU is not OSF-specific attribution, and private memory is not
VRAM. `comparison-final/summary.json` retains all metrics, per-repeat values,
exclusions, signatures, and provenance; `comparison-final/summary.md` is the CPU
table generated directly from those values.

## Recovery responsiveness

| Backend | Replacement document before -> after | Maximum native snapshot age before -> after | Stale observations before -> after |
| --- | ---: | ---: | ---: |
| WebView2 | 17,266 -> 18,076 ms | 6,187 -> 102 ms | 20/73 -> 0/75 |
| CEF | 17,397 -> 17,668 ms | 6,185 -> 107 ms | 20/74 -> 0/72 |

The candidate keeps native game-drain snapshots fresh while the helper is being
retired. This is consistent with moving failed-host retirement off the runtime
tick (`4465462d`). Replacement latency itself did not improve; it includes the
heartbeat timeout, retirement, and browser startup. Each entry is one injected
stall trial per variant, not a latency distribution or rendered-frame benchmark.

The case logger originally dereferenced an absent page's `documentId`, losing
its summary records during replacement. The underlying harness retained every
native status response. `recovery-native-responses.json` reconstructs the interval
from those files, records their paths/hashes, and uses native observation time
(`snapshotAtUtcMs + snapshotAgeMs`) between suspension and replacement. Every
sample matches its owned game PID and creation time, remains loaded and unpaused,
and the maximum observation gap is 306 ms. Original case evidence is unchanged.
This corrects earlier partial summaries that understated the baseline stall.

The logging defect is fixed in `0e75b3fc`; null, missing, empty, and replacement
document checks pass under PowerShell strict mode. The repair changes observation
logging only. The completed live runs used the earlier logger and are evaluated
using the retained raw responses, not represented as reruns of that repair.

## Fixes and retained unsuccessful attempts

The live CEF failure showed a reported `ERR_NAME_NOT_RESOLVED` being cleared by a
subsequent loading-state update. `ccee7b6e` clears the error only on a new loading
transition, and adds a nonempty fallback for CEF's optional empty description.
Both final backends passed the navigation scenario after rebuilding the core.

Validation-tool fixes add backend verification before leasing profiles,
authorized foreground focus, camera/render parity, immediate pause rejection,
active-build detection, audio-route readiness, and null-safe recovery logging.
They are committed separately from runtime fixes.

Earlier attempts remain available as evidence: a disabled Browser TV ESM in the
first private fixture; focus/vanity-camera mismatches; a paused CEF run; an
interrupted WebView2 idle capture followed by a different camera; browser process
churn; an overpacked fault sequence exhausting the designed recovery budget; and
the CEF audio-readiness race. Fault trials now wait 65 healthy seconds after a
successful recovery. None of those mismatched captures enters the final table.

PresentMon ETW acquisition was denied by Windows permissions. No elevation or
group membership was changed to obtain it. Consequently FPS, frame pacing,
GPU-engine utilization, VRAM, and context-switch improvements are unmeasured.
Physical controller interaction, fullscreen rendering cost, hardware endpoint
hotplug, subjective audio quality, DRM/iframe media, and broad gameplay remain
outside this fixture. The missing-domain navigation test is not live postcommit
transport-error coverage.

## Restoration and repository state

`final-restoration.json`, completed at 2026-10-21T19:57:28Z, verifies:

- All eight original production payload files retain their recorded hashes.
- Eleven root build/configuration files are restored from the task backup.
- The original WebView2 helper cache is restored and hash-matched.
- Default is selected in MO2, and its three mod/plugin/load-order lists match.
- All twelve browser-profile leases are restored, with original file hashes
  verified by their lease receipts; all twelve owned temporary MO2 setups are gone.
- Five unrelated files match their original hashes. The remaining dirty
  `HostApp.cpp` insertion/deletion lines match the original saved patch exactly;
  committed review changes in that file remain intact.

The owned games exited normally and their helpers exited before restoration.
The user authorized foreground focus only; no keyboard, mouse, or controller
input was injected. The Intimacy chat finished its existing run and held further
launches during comparison/restoration; no foreign game was stopped.

The new navigation fix is committed and validated in private payloads. Original
production binaries were restored as requested, so this additional fix is not
installed over the original setup. No push, tag, or publication was performed.
