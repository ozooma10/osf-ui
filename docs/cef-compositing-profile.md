# CEF compositing profile — October 17, 2026

Validation is in progress. The implementation builds and passes the native and
bridge suites. The final repeated desktop comparison and packaged in-game runs
still need an uninterrupted desktop with other Starfield sessions closed.

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
The final fixture implementation is committed as `f1dc9300`. Both packages were
rebuilt and verified again from that clean revision under
`build/resume-20261017/packages-f1dc9300/`; the shared DLL hash is
`5D040C8563AAA56CF2025A8B2E719E28FEA58B8CD9E61255CE245CE624CCB938`.
The filename's `final` label identifies this candidate build, not acceptance.

| Candidate | ZIP SHA-256 | Game acceptance |
| --- | --- | --- |
| `OSF-UI-v2.0.0-cef-handoff-final-webview2.zip` | `48421EA5E103A0F312170655E868F75C738F3058986ACAC04A21398370BA121B` | Pending |
| `OSF-UI-v2.0.0-cef-handoff-final-cef.zip` | `3A875D26725E63C52BA3DACAB0CE6E0E3B7E29CDD8547CB44F7A469D05BE0171` | Pending |

`build/resume-20261017/validation.json` records the actual binaries, failed
evidence, inspected captures, packages and remaining gates. Repeated optimized
helper lifecycle, 1080p/1440p performance, dirty/fullcopy attribution and both
exact-package game runs remain unfinished. No CEF speed or memory improvement
is claimed from this continuation. Nothing was pushed or published.
