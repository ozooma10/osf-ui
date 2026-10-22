# World-screen idle CPU and browser memory investigation, 2026-10-21

This follow-up investigates the mixed steady-state results in
[the earlier game validation](world-screens-game-validation-2026-10-21.md).
The production change skips Browser TV DOM and storage writes when only audio
diagnostics changed. It preserves the latest complete state and still updates
navigation, loading, mute, audibility, screen count and landing controls.
In-game measurements confirm elimination of redundant DOM mutations, but the page
fix shows no consistent total CPU improvement and essentially unchanged browser
memory relative to the deployed runtime. WebView2 idle CPU direction reverses
between launch pairs; tone CPU is higher in both. The older review's roughly
16 MiB memory increase reproduces against the original runtime and remains a
separate finding; the page fix does not remove it.

## Revisions and method

- Original review baseline A: `49d1a0af`.
- Previously validated and deployed runtime B: `ccee7b6e`.
- Candidate C: the same B native binaries, with `chrome.js` from `c979268b`.
- The six pre-existing dirty/untracked files were recorded by hash before work.
  The pre-existing WebView2 IPC changes remain identical in the native payloads.

Fresh private MO2 profiles and leased browser profiles use the same save, two
1920x1080 TV surfaces, settings, page observer and synthetic 997 Hz tone. Each
session warms for 120 seconds and attempts three 30-second captures per workload.
CPU is sampled at 1 Hz. Workloads are home with tone off, and home with both TVs
emitting the tone; they are not video-decoding performance benchmarks.

The accepted follow-up captures run in the background, as requested, while normal
desktop use remains allowed. The game stays loaded and unpaused. Window captures
are 3442x1441 and runtime capture/viewport dimensions are 3440x1409. Both variants
use matching display/quality preferences, including enabled dynamic resolution.
The earlier report used foreground gameplay and 2560x1409 output, so its absolute
values are not pooled with these measurements. Developer diagnostics are enabled
for both variants.

The analyzer rejects changed process/document identities, stale page/native
observations, menus/pause, focus/cell/position/camera changes, and unmatched
workload or rendering signatures. Active local builds abort capture. The camera
fixture disables vanity mode after load with the game's native INI setter, only
in the disposable session; actual observed camera stability is still checked.

CPU 100% means one logical core. Browser CPU and private memory include the host
and Chromium children. Private bytes exclude VRAM, JS heap is approximate, and
whole-game CPU cannot be attributed to OSF UI. PresentMon/ETW frame timing remains
unavailable; these results establish no FPS or frame-pacing improvement. Three
intervals in a process launch are repeated observations, not independent trials
or a statistical significance test.

## Source diagnosis

Audio diagnostics carry changing poses/counters in `browser.state`. The page did
not display those fields, but its outer `apply()` still rewrote toolbar fields,
body classes, document title and history storage. The landing list already had
its own render key. The new outer key excludes only `audio`, retaining all other
state fields so new presentation state is not silently ignored.
The measured benefit concerns diagnostic-mode traffic. Non-developer host state
already omits most changing diagnostic fields. Hidden toolbar mutations also do
not necessarily produce a new rasterized frame, so eliminating them need not
produce a measurable whole-browser CPU decrease.

Native geometry updates already deduplicate equal layouts, so another geometry
cache would add complexity without fixing this issue. Review of the original
WebView2 host diff found navigation lifecycle state and a cheaper audio failure
query, with no added steady-state browser/controller or frame-buffer allocation.
That source inspection does not by itself exclude a Chromium allocation change.

The historical WebView2 memory increase was concentrated in the GPU process:
idle private memory 116.941 -> 129.215 MiB and tone 115.937 -> 129.225 MiB. Host
memory was nearly unchanged (idle 55.618 -> 55.973 MiB). Short samples cannot
establish a leak or prove which Chromium subsystem allocated those pages.
In the first follow-up WebView2 pair, GPU private memory was effectively flat
within each interval (maximum positive drift 0.270 MiB). One candidate idle
interval gained 12.777 MiB overall, of which 10.219 MiB was browser-main allocation;
GPU drift was only 0.063 MiB. Preserve this variability rather than describing a
short overall memory increase as a retained-page or GPU leak.

## Functional and regression validation

The actual chrome script passes three Node tests. A negative control using the
old script fails the audio-only write assertion: 100 diagnostic updates cause
2,300 additional tracked DOM/storage writes. The candidate causes zero. These
test counters differ from the browser MutationObserver records measured in game.

Both candidate backends passed multiple TVs (2 -> 1 -> 0 -> 2), save/load,
navigation stop/failure/home, remote WebM playback, mute/unmute, and a 30-second
sustained tone. Screenshots were inspected and show playback on both TVs.

| Candidate backend | Accepted functional run | 997 Hz endpoint RMS |
| --- | --- | ---: |
| WebView2 | `20261022-000330-605-current-webview2` | 0.012249 |
| CEF | `20261022-001939-389-current-cef` | 0.022155 |

Each sustained sample recorded 1,440,480 stereo frames at 48 kHz with zero
discontinuities, timestamp errors or invalid samples. CEF's PCM generation stayed
stable and dropped-packet deltas were zero. Endpoint measurements include desktop
audio; the controlled frequency and route/mute checks support source attribution.
These separate runs are not a backend loudness-equivalence test.

The synthetic tone is explicitly restarted after save/load; this demonstrates
restoration, not gap-free audio across browser destruction. Host fault recovery
was validated in the preceding review on these unchanged native binaries and was
not repeated for this page-only fix. No fresh subjective listening, controller
hardware or DRM acceptance is claimed.

## Deployed runtime B versus page fix C

| Backend / order / workload | Browser CPU B -> C | Relative CPU change | Browser private MiB B -> C | DOM records/s B -> C |
| --- | ---: | ---: | ---: | ---: |
| WebView2, B then C, idle | 7.459 -> 8.201 | +10.0% | 397.733 -> 398.185 | 25.867 -> 0 |
| WebView2, B then C, tone | 8.898 -> 9.595 | +7.8% | 395.259 -> 397.042 | 23.463 -> 0 |
| WebView2, C then B, idle | 7.881 -> 7.041 | -10.7% | 398.998 -> 398.618 | 25.765 -> 0 |
| WebView2, C then B, tone | 7.451 -> 8.211 | +10.2% | 403.911 -> 403.185 | 25.760 -> 0 |
| CEF, C then B, idle | 3.304 -> 3.467 | +4.9% | 363.634 -> 362.878 | 0 -> 0 |
| CEF, C then B, tone | 5.392 -> 6.074 | +12.6% | 357.258 -> 359.482 | 10.820 -> 0 |

Each row has three matched intervals. CEF's CPU ranges overlap substantially:
idle B 2.986-3.948 versus C 3.151-4.582; tone B 3.943-5.877 versus C 3.518-7.035.
Its idle state produces no continuing audio-only events, explaining the absence
of baseline idle DOM mutations. WebView2 continues sending those diagnostics
while the tone is off. The proven improvement is eliminated redundant page work,
not a demonstrated reduction in total browser CPU or memory.

Reversing WebView2's launch order also reverses its idle CPU direction. Pair 2
uses idle before tone; pair 1 uses tone before idle. The candidate's idle median
itself varies from 8.201 to 7.041 across launches. Both WebView2 tone comparisons
are higher for C, by 0.697 and 0.760 percentage points of one core; retain that
negative observation rather than presenting the DOM reduction as a CPU win.
These process-level measurements do not isolate the page handler's CPU cost.
For B -> C, total browser private memory changes remain below 1% on both backends.

| Comparison | Baseline run | Candidate run | Summary directory |
| --- | --- | --- | --- |
| WebView2 pair 1, tone first | `20261021-235613-525-baseline-webview2` | `20261022-000330-605-current-webview2` | `background-webview2-pair1` |
| CEF pair 1, tone first | `20261022-003208-308-baseline-cef` | `20261022-001939-389-current-cef` | `background-cef-pair1` |
| WebView2 pair 2, idle first | `20261022-004523-388-baseline-webview2` | `20261022-003857-488-current-webview2` | `background-webview2-pair2` |

Each comparison has 12 valid captures and six matched pairs. Summary directories
are relative to `build/world-screen-idle-review-20261021/` and contain JSON metrics,
per-capture process memory drift, admission signatures and generated Markdown.

## Original runtime A control

The original WebView2 payload was rerun as
`20261022-005147-506-baseline-webview2`, with the same background, idle-first
conditions as candidate pair 2. All 12 captures are valid and form six matched
pairs against `20261022-003857-488-current-webview2`. Results are in
`background-original-webview2-control`. The candidate captures are reused for
this separate comparison, not counted as independent additional measurements.

| Workload | Browser CPU A -> C | Browser private MiB A -> C | GPU private MiB A -> C |
| --- | ---: | ---: | ---: |
| Idle | 7.355 -> 7.041 (-4.3%) | 382.388 -> 398.618 (+4.2%) | 115.945 -> 126.738 |
| Tone | 8.255 -> 8.211 (-0.5%) | 387.665 -> 403.185 (+4.0%) | 115.978 -> 126.813 |

The idle CPU increase does not reproduce in this control. The memory increase
does: approximately 16 MiB overall, of which 11 MiB is in the GPU process. Renderer
private memory adds roughly 4-5 MiB; host private memory adds about 1.1 MiB.
This is a repeated allocation difference, not evidence of unbounded growth.

Both versions use WebView2 155.0.4283.62, matching browser/GPU flags, two hosted
views, a 1920x1080 capture and a four-slot shared texture ring.
Page manifest and HTML differences are line endings only. The old runtime reports
one logical screen for two TVs; the new runtime correctly reports two. These
checks rule out a changed browser version or requested capture size, but do not
identify the allocator responsible for the extra GPU-process private bytes.

A final helper-only control, `20261022-010013-899-baseline-webview2`, combines
current core/extension/page files with the original `49d1a0af` WebView2 helper.
The isolated payload and its explicit provenance are under
`payloads/host-control/webview2/`. Its comparison against C is in
`background-webview2-host-control`: 11 valid captures and five matched pairs.
Idle repeat 21 is excluded because its process set changed; it was not replaced.

With the old helper, accepted GPU private-memory medians are 128.422 MiB idle
and 128.441 MiB with tone, still above original A's approximately 116 MiB. Total
private memory is 393.979/397.590 MiB versus C's matched 398.764/403.185 MiB.
Thus reverting the helper alone does not remove the GPU allocation difference.
This narrows the investigation but does not attribute the remaining cost to a
specific core, extension or rendering change. The extra memory remains unresolved;
no speculative native change was deployed.

## Deployment and restoration

At 2026-10-22 01:06:56 UTC, only the validated Browser TV `chrome.js` was deployed
to the normal World Screens mod. SHA-256:
`8088861000FF9648F2BDA281EEC54F7B50286A431D41885689A67C9F7B51E1DC`.
The original file is backed up in `deployment-backup/` under the follow-up evidence
root. Normal native binaries remain at their previously validated versions.

The final check verifies all 32 protected files against their starting hashes,
except the intentionally updated chrome file, which matches the tested candidate.
It also verifies all six unrelated dirty/untracked files byte-for-byte. The global
WebView2 helper cache is restored to `FBE0B572...` after the old-helper controls.
MO2 had normalized Default's `plugins.txt` while closing; its exact original bytes
were restored after checking that only the known normalization was present.
Default is reopened, original mod enablement and settings are preserved, all 11
browser-profile leases are restored, and registered temporary MO2 directories are
removed. No owned test game or browser helper remains running.

`deployment-restoration.json` contains full file hashes and the deployment receipt.
Production fix: `c979268b`. Measurement/fixture commits: `002024b7`, `a8a435ea`,
`792053f0`, `c532bb4e`, `3387c9f9`, and `97a68a88`.

## Evidence and retained unsuccessful attempts

Raw runs live under `build/world-screen-validation/runs/`. Follow-up summaries,
negative-control output, payload receipts and preservation records live under
`build/world-screen-idle-review-20261021/`.

- `20261021-234113-707-baseline-webview2`: rejected focus/camera motion and build
  interference; zero accepted captures.
- `20261021-234935-220-baseline-webview2`: focus/camera changes and a DataMenu
  opening; the session was stopped normally.
- `20261022-001135-195-current-cef`: camera orbit and local compiler interference;
  retained separately from the successful CEF run.
- Immediate MO2 reopen/close races prevented two following sessions from starting.
  Successful intermediate runs can now leave MO2 closed after temporary-profile
  cleanup, avoiding needless reopening of Default between comparisons. Failure
  cleanup still attempts to reopen Default.

No failed capture was silently replaced inside its run directory. Browser-profile
leases are hash-restored after each owned game exits normally. The runtime camera
setter replaces an ineffective startup-only INI attempt; it does not modify the
user's permanent camera settings.
