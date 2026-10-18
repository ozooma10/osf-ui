# CEF compositing profile — October 17, 2026

The same-PC validation passed: repeated lifecycle, production throughput,
dirty/full-copy diagnostics, and both exact final packages in-game with the
default 120-second soak. The measurements do not establish a general CEF speed
or memory improvement; WebView2 remains the default and CEF the bundled alternative.

This report describes the optimization handoff. The subsequent
[2.1 release record](release-2.1.md) covers the final production Settings packages,
later input/frame fixes, original-mod diagnoses and completed scoped edition-switch
round trip, including remaining consumer limitations.

The original-PC sections below are historical checkpoints; the continuation and
final validation sections record the new PC's results separately.

## Attribution

The current event-driven helper was profiled separately from the production
performance runs on a Ryzen 9 7950X / RTX 3080 at 2560×1440. Diagnostic code is
generated under `build/cef-audit`; it is not compiled into shipped helpers.

With three animated views, each browser produced about 60 paint callbacks/s, and
the helper published about 60 composites/s. The independent paint and output
cadences sometimes sampled the top view twice, reducing distinct output frames.
Two fresh, uninstrumented six-second baseline trials delivered 47.7–49.7 new
frames/s. Instrumentation itself changed delivery, so its throughput must not be
used as the baseline.

A separate GPU-query run measured mean copy execution of 0.077 ms and composition
of 0.159 ms. The copy's signal/flush/completion path took 0.717 ms of wall time on
average. These overlapping measurements cannot be added together. Most browser
CPU was outside the host process. Removing the borrowed-texture completion wait
would violate CEF's resource lifetime contract; it remains intact.

## Changes

- Coalesce recently animating layers for at most four milliseconds after the
  shared output deadline, preserving the next 60 Hz deadline and immediate bridge
  scheduling. Static views cannot hold up animation indefinitely. Two exploratory
  production trials delivered 55.6–59.3 distinct frames/s with three views; the
  final repeated comparison remains pending.
- Stop hidden browsers painting, ignore their late callbacks, and release copied
  view/popup textures. Instrumented five-cycle hide/show testing recorded 42.19 MiB
  of owned paint pixels while three 1440p views were visible and zero when hidden,
  returning to 42.19 MiB after every show. This excludes the output ring, driver
  overhead and Chromium allocations. It is not a process RAM saving: private
  commitment remained high and varied across reopen cycles.
- Retry a reveal refresh once after 34 ms if no post-reveal paint arrived.
  Callback tracing caught a static view painting before its reveal sentinel,
  then receiving no additional callback from the immediate refresh. The retry
  checks document token, visibility, loading and close state; pre-reveal pixels
  are never accepted as a fallback.
- Expand real-helper tests to verify transparent overlap, z-order, hide/reshow,
  destruction of the top layer, and lower-layer animation under static overlays.
  Failed tests retain pixels, an epoch and a screenshot where available.

Same-site renderer reuse saved approximately 75–85 MiB in the three-view
experiment but did not consistently improve delivery. It is not enabled in
production because it would couple renderer-thread responsiveness across views.
Chromium describes the [process reuse tradeoff](https://chromium.googlesource.com/chromium/src/+/main/docs/process_model_and_site_isolation.md).

## Evidence and remaining work

Local artifacts are retained under `build/cef-optimization/`:

- `before-audit-local/` and `before-report/`: original production and timed helper.
- `reuse-audit/`: same-site process reuse experiment.
- `coalesced-performance/`: exploratory uninstrumented optimized runs.
- `gpu-profile/`: GPU timestamps, separate from throughput comparisons.
- `retention-cycles/`: original package versus instrumented optimization, five
  hide/show cycles and per-process memory snapshots.
- `layers-baseline/`: both original helpers passed all layer checks available then.
- `lifecycle-final/`, `reshow-repro/`, `reshow-trace/`: retained static-reveal failure
  and diagnostic callback ordering.
- `reveal-fix-lifecycle/cef-1440p-r1/`: clean pass of all 15 helper lifecycle checks
  after the refresh fix. The next repetition was invalidated by another Starfield
  session starting; it is not counted as final acceptance.
- `native-tests.log`, `web-tests.log`: all 28 registered native tests and nine
  bridge/compatibility tests passed; the updated frame-pacer test also passed.

Pending: repeated clean renderer/lifecycle runs, final performance comparison of
both backends, exact-package in-game acceptance, and final validation manifests.
No publication is authorized or performed. Desktop output rates are not game FPS;
private commitment is not resident RAM or VRAM.

Both candidate packages were built/staged from clean implementation commit
`575848a49fd95896b34933af57f47d0ccca3f764` and verified locally under
`build/cef-optimization/packages/`. Common payloads are byte-identical; actual
DLL API acquisition, all 55 compiled Papyrus signatures, CEF runtime manifest and
ZIP content hashes passed. These packages have not been tested in-game or deployed.

| Candidate | ZIP SHA-256 |
| --- | --- |
| `OSF-UI-v2.0.0-cef-opt-webview2.zip` | `1081B17675D45A6308EAABD536C2B829A5E16C6AFD66B9E1F9C8D8CF9FEC77CA` |
| `OSF-UI-v2.0.0-cef-opt-cef.zip` | `C156DC9A916971328F0E9090DF1DBE6ACF99528F9291291DE424BA63AABFA99F` |

`build/cef-optimization/validation.json` records this checkpoint. Other Starfield
sessions repeatedly started during validation; the runner retained and rejected
the contended repeat. Further renderer and game tests await a clear desktop.

## Follow-up checkpoint

`build/cef-next/baseline-lifecycle/` completed three clean 1440p sessions per
backend, each passing all 15 lifecycle checks against the existing production
helpers. The new mixed benchmark adds independently observed menu/HUD/static
markers, with 10 and 30 Hz HUD cases. It compiles; complete performance validation
is pending. `baseline-performance/webview2-1440p-r1/contention.json` rejects the
first performance session because another Starfield session started during it.
The already-built candidate ZIPs still need their packaged in-game acceptance.

Dirty copying now preserves the owned surface outside CEF's damaged region. It
unions/clips damage in coded texture coordinates, translates into the visible
surface, and copies the full frame after allocation, a changed visible rectangle,
missing/discontinuous capture counters, or unusable damage. View and popup
histories are independent and clear with their textures. The borrowed-resource
completion wait is unchanged. The host builds and all 20 damage-planner checks
pass, including pixel preservation after a dropped capture and erased content.
Diagnostics now record copied MiB and the fraction of the visible frame copied.
Runtime correctness and performance remain to be measured on a clear desktop.

Publication scheduling now owns dirty work and its idle/queued/blocked/stopped
state in `FrameSchedule`; normal and coalesced wakes share one timer path. The
reveal gate uses explicit sentinel/paint/ready phases in `RevealState`, resetting
on hide, navigation and renderer failure. Duplicate sentinels cannot restart a
completed reveal, and delayed refreshes remain bound to their original token.
All 29 registered native suites and nine bridge/compatibility tests pass; the
frame pacing/scheduling/reveal test contains 45 checks. Production and diagnostic
helpers build successfully. These offline checks do not replace helper/game runs.

## Same-PC continuation — Ryzen 9 5900X / RTX 3080

The new checkout fetched `6cba68efc3c84072530d381a96f83167e9bc74bd` directly
from origin and preserved local `dev` on a separate
`osf/cef-optimization-handoff` branch. The named transfer bundle was not found;
no bundle import or verification was needed to obtain the already-published tip.
The pinned CEF archive passed SHA-256 verification. Both production helpers,
the shared DLL, and the benchmark rebuilt locally; all 29 native and nine bridge
tests passed. A separate CEF runtime was rebuilt from `575848a` under
`build/resume-20261017/baseline-cef`, using an archived source snapshot. It does
not replace the current helper or identify the original machine's ZIP bytes.

The first baseline comparison failed WebView2 canvas marker validation. Its
capture was inspected and showed rendered content. The diagnostic retry recorded
raw marker `0x1000d5` where the page's serial encoded red channel `0x11`:
capture color conversion can shift that channel down to `0x10`. Raw RGB serials
therefore cannot reliably establish freshness on this desktop. Evidence remains
in `baseline-performance/` and `marker-probe/` under the continuation directory;
neither is an accepted performance result.

The first correction used two pixels with channel nibbles spaced 16 levels
apart. `marker-fixed-smoke/` rejected that approach: a nominal low red channel
beside bright green was captured as 49, beyond small quantization error. The
replacement uses eight binary RGB blocks in the existing 32x32 marker. It
preserves the full 24-bit serial and rejects midrange channels or nonopaque
samples. `binary-marker-smoke/` passed WebView2 marker validation through the
mixed10 case, observing about 10 HUD updates/s and an unchanged static marker.
It then correctly failed the remaining hard-coded HUD color expectation:
the captured bar was RGB `(37,163,93)`, rather than `(19,164,85)`.

The HUD probe now compares same-frame always-filled bar and exposed-background
references, requiring those references to remain distinct. This still rejects
trails, missing expanded bars and an absent HUD. Marker and HUD checks pass
offline, including the captured colors, serial byte boundaries, row padding,
invalid alpha and corrupt channels. The final `color-probe-smoke/` passed all
six workloads against both baseline helpers at 1920x1080, with one second warmup
and three seconds measured per phase. All mixed markers, HUD erase probes and
static-layer checks passed, and both helpers shut down cleanly. The WebView2
and CEF mixed30 captures were inspected: each showed the menu, HUD and static
translucent overlay. The CEF helper was the isolated `575848a` build; WebView2's
production source is unchanged between that revision and the handoff tip.
All failed captures were inspected and retained. These short fixture checks
are not an accepted repeated performance comparison. Measurements using
different encodings must not be combined.

Unrelated Starfield sessions started before both `marker-fixed-smoke`'s first
attempt and `color-probe-smoke`'s first attempt; the guards refused those attempts.
After the completed smoke comparison, another unrelated session blocked the
baseline lifecycle invocation before it created an evidence directory. No
unrelated game was closed or driven. Runtime checks require a clear desktop.

The existing harness checkout predates native console dispatch. Its fetched
`8a57a3d` revision is isolated in sibling `OSF Test Harness UI Validation`;
the original harness branch is unchanged. The local disposable save and SFSE
sidecar match their pinned hashes. The separate harness build and all 25 offline
background-control checks pass. Packaged game acceptance remains pending.

The diagnostic helper and native/Papyrus game consumer also rebuilt successfully.
Both candidate packages from clean revision `be91d407` passed DLL API acquisition,
all 55 compiled Papyrus signatures, CEF manifest/ZIP checks and common-payload
byte equality. They are retained in `build/resume-20261017/packages-be91d407/`.
The marker/HUD fixture correction is committed as `f1dc9300`. Both packages were
rebuilt and verified again from that clean revision under
`build/resume-20261017/packages-f1dc9300/`; the shared DLL hash is
`5D040C8563AAA56CF2025A8B2E719E28FEA58B8CD9E61255CE245CE624CCB938`.
The filename's `final` label identifies this candidate build, not acceptance.

| Candidate | ZIP SHA-256 | Game acceptance |
| --- | --- | --- |
| `OSF-UI-v2.0.0-cef-handoff-final-webview2.zip` | `48421EA5E103A0F312170655E868F75C738F3058986ACAC04A21398370BA121B` | Superseded; untested |
| `OSF-UI-v2.0.0-cef-handoff-final-cef.zip` | `3A875D26725E63C52BA3DACAB0CE6E0E3B7E29CDD8547CB44F7A469D05BE0171` | Superseded; untested |

These earlier ZIPs are retained as superseded candidates. The final packages and
completed desktop evidence are recorded below. No CEF speed or memory improvement
is claimed from copied area alone. Nothing was pushed or published.

## Final desktop validation on the new PC

Commit `3282d02c` also corrected the lifecycle fixture's saturated alpha colors:
WebView2 capture color conversion changed the expected overlap colors. The failed
`current-lifecycle/webview2-1080p-r1` capture was inspected and retained. Neutral
grays now test the same overlap, z-order, hide, reshow and destruction behavior
with the original two-level tolerance. The private game profile also detects
competing OSF UI/Settings DLL owners by payload, including release folders named
`OSF-UI-v2.0.0` or `OSFSettings`; it does not edit the source profile.

All evidence below is under `build/resume-20261017/`, with no game contention:

- `baseline-lifecycle-final/`: six isolated `575848a` CEF sessions, three at each
  resolution, all 15 checks and normal shutdown passed.
- `current-lifecycle-final/`: twelve current sessions, three per backend and
  resolution, all 15 checks and normal shutdown passed.
- `baseline-performance-final/`: six baseline CEF sessions, six workloads each.
- `current-performance-final/`: twelve current sessions, six workloads each.
- `copy-audit-final/`: six diagnostic sessions at 1440p, alternating dirty and
  forced-full copying across three repetitions, with GPU queries and no tracing.

Production comparisons use three seconds warmup and ten seconds measured per
workload. The diagnostic comparison uses two plus six seconds. Every accepted
run has valid markers, zero invalid HUD pixels, zero static-overlay changes and
a clean zero exit. Current-production HUD freshness spans 9.997–10.098 updates/s
for the 10 Hz case and 29.979–30.098 for 30 Hz. Diagnostic ranges are
9.984–10.162 and 29.954–30.164. Representative mixed captures were inspected for
every helper/resolution and both diagnostic modes; they show correct menu, HUD
and translucent overlay content. The manifest lists individual reviewed files.

### Production measurements

Each cell is **new frames/s / CPU % of one logical core / private commit MiB**,
the median of three fresh sessions. Private commitment is neither resident RAM
nor VRAM; these desktop output rates are not Starfield FPS. The baseline is the
same-PC rebuild of `575848a`; current CEF includes the handoff's dirty-copy and
state changes. WebView2 production source is unchanged between those revisions.

| Resolution / workload | CEF baseline | Current CEF | WebView2 |
| --- | ---: | ---: | ---: |
| 1080p / idle | 0.00 / 2.65 / 315.8 | 0.00 / 2.97 / 319.3 | 0.00 / 0.47 / 255.5 |
| 1080p / canvas | 60.09 / 23.59 / 335.1 | 60.09 / 18.90 / 338.5 | 57.10 / 17.18 / 260.4 |
| 1080p / dom | 60.08 / 36.55 / 354.6 | 60.06 / 32.32 / 358.0 | 56.04 / 27.31 / 290.9 |
| 1080p / three_views | 55.19 / 57.64 / 563.0 | 59.47 / 62.25 / 559.3 | 57.86 / 43.89 / 469.2 |
| 1080p / mixed10 | 60.05 / 45.12 / 559.8 | 59.70 / 38.11 / 562.5 | 57.49 / 24.53 / 509.6 |
| 1080p / mixed30 | 59.09 / 42.18 / 534.4 | 59.80 / 40.31 / 536.6 | 57.10 / 21.55 / 500.6 |
| 1440p / idle | 0.00 / 3.59 / 364.4 | 0.00 / 4.68 / 363.7 | 0.00 / 0.78 / 274.6 |
| 1440p / canvas | 60.09 / 20.62 / 384.0 | 60.09 / 21.87 / 383.0 | 57.85 / 15.46 / 279.5 |
| 1440p / dom | 60.05 / 38.88 / 407.0 | 60.06 / 32.32 / 406.5 | 57.67 / 26.08 / 312.8 |
| 1440p / three_views | 59.28 / 61.23 / 676.2 | 58.48 / 59.04 / 672.8 | 57.45 / 42.32 / 518.6 |
| 1440p / mixed10 | 60.08 / 41.86 / 673.6 | 60.08 / 44.36 / 669.1 | 57.91 / 21.53 / 556.7 |
| 1440p / mixed30 | 59.69 / 42.65 / 648.3 | 59.89 / 45.61 / 647.7 | 56.87 / 32.03 / 548.1 |

The data does not establish a general CEF speed or memory improvement. In the
three-view case, baseline/current output ranges were 54.38–60.08 / 55.81–59.48
at 1080p and 56.26–60.01 / 49.38–58.80 at 1440p. The slower current 1440p third
repetition is retained, with no invalid frames or contention reported. Separate
diagnostic repeats reached 56.89–58.98, but instrumentation changes timing and
cannot explain or replace that production result. Scheduling consistency remains
a performance limitation, not a demonstrated rendering corruption.

CEF mixed-view medians stayed near 60; WebView2 was around 56–58 here and generally
used less CPU and private commitment. CEF/WebView2 fresh-profile first-content
medians were 653/738 ms at 1080p and 644/717 ms at 1440p; shutdown medians were
122/41 ms and 123/36 ms. These warm-cache, short runs do not rank long-session or
real-mod behavior. Detailed ranges, bridge RTT, process samples and provenance
are retained in each `summary.json` and `comparison.html`.

### Dirty-copy attribution

Here `baseline` means the **current instrumented dirty-copy helper**, not the old
production CEF build. Each pair below is **dirty / forced full**, the median of
three per-run means. Both modes use the same helper and scheduler.

| Workload | Copied fraction | MiB per paint | GPU copy ms | Fence/flush/wait ms |
| --- | ---: | ---: | ---: | ---: |
| canvas | 1.0000 / 1.0000 | 14.062 / 14.062 | 0.185 / 0.175 | 0.878 / 0.912 |
| dom | 0.9859 / 1.0000 | 13.865 / 14.062 | 0.207 / 0.246 | 0.881 / 0.913 |
| three_views | 1.0000 / 1.0000 | 14.062 / 14.062 | 0.121 / 0.124 | 0.907 / 0.876 |
| mixed10 | 0.9879 / 1.0000 | 13.893 / 14.062 | 0.182 / 0.153 | 0.842 / 0.820 |
| mixed30 | 0.9906 / 1.0000 | 13.931 / 14.062 | 0.146 / 0.207 | 0.792 / 0.949 |

Static idle produced no measured paint copies. Full-canvas cases copied the
whole surface; the DOM and mixed cases copied about 98.6–99.1% per callback on
average. The small visible HUD therefore does not demonstrate a small submitted
copy in this fixture. Mixed10 copied roughly 5,849 versus 5,920 MiB per measured
phase; mixed30 roughly 7,536 versus 7,594 MiB. Callback counts can differ slightly,
so the per-paint fraction is the direct area comparison.

For mixed10/mixed30, dirty/full median CPU was 44.72/41.87 and 48.14/46.29;
private commitment was 673.0/684.0 and 673.8/684.3 MiB. These observations do not
establish a CPU or memory benefit. Mixed30's median mean GPU-copy time and
fence/flush/wait were lower with dirty copying, while mixed10's were higher.
GPU timestamps, completion wait and enclosing scopes overlap and are not summed.
The borrowed texture completion wait remains intact. Correctness passed in both
modes, but a broad performance claim or removal of that wait is not justified.

### Final package acceptance

The packages in `build/resume-20261017/packages-3282d02c/` were rebuilt from clean
implementation revision `3282d02c`. DLL API acquisition, all 55 compiled Papyrus
signatures, runtime manifests, ZIP hashes and identical common payloads passed.
Both retain shared DLL SHA-256
`5D040C8563AAA56CF2025A8B2E719E28FEA58B8CD9E61255CE245CE624CCB938`.

| Package | ZIP SHA-256 | Game acceptance |
| --- | --- | --- |
| `OSF-UI-v2.0.0-cef-handoff-final-webview2.zip` | `BA04E10B30DA30EAB7BA0CCDF148353316DB44CCE9CACC635AF417C153F5E684` | Passed |
| `OSF-UI-v2.0.0-cef-handoff-final-cef.zip` | `47F0AB82DD59CDB5923C80D330E54A24E50929A28E7995F1D8107DA15510F438` | Passed |

Game evidence is under `build/backend-validation/game/`:

- `20261017-170611-841-webview2-stock/`: 92 assertions passed.
- `20261017-170943-978-cef-stock/`: 94 assertions passed, including the loaded CEF
  DLL and runtime hashes.

Both ran on Starfield 1.16.244.0 at 2560×1440 with stock graphics and frame
generation disabled, using private profiles/saves and harness `8a57a3d`. Both
passed backend selection, public native and actual Papyrus VM requests,
keyboard/mouse/select/relative-drag input, focus recovery, retained state and
reload, legacy ABI/web compatibility, eight reopens, HUD movement, engine pause
ownership, actual save/load, owned-host failure/recovery and 24 five-second soak
checks. No device loss or hook failure was reported; helpers exited cleanly when
their owned games closed. All nine captures per backend were inspected,
including the native popup, HUD/menu overlap, reload/recovery and final gameplay.
The final captures show the browser overlays removed. Unrelated game sessions
were never interrupted, and live installed mod/profile contents were not changed.

`build/resume-20261017/validation.json` binds results to package and binary hashes,
failed evidence and capture reviews. The original PC's exact `575848a` ZIPs were
not transferred; its rebuilt helper is desktop baseline evidence only. Stock
acceptance does not establish Luma/HDR/frame-generation, controller/IME,
multi-GPU, long-session or real-mod performance. No production code change was
needed after the handoff; continuation fixes concern fixture color conversion
and private-profile portability. One shared DLL/API and the existing backend
defaults remain. Nothing was pushed or published.
