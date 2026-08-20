# UI framework performance toolbench

This kit compares OSF UI, Carbon UI, and a no-framework Starfield baseline
with an optional dual-framework fixture and external profilers. It records
repeated runs, rejects a launch with the wrong framework DLLs, validates the
actual page viewport and SHA-256, groups only exactly matched conditions, and
produces median comparison tables.

Carbon UI runs its loader, core, Ultralight, and WebCore inside Starfield. OSF
UI also has in-process work but renders through an OSF UI host and descendant
WebView2 processes. Therefore:

- whole-system, game-process, frame-pacing, and matched-baseline deltas are the
  fair headline metrics;
- OSF UI host/WebView2 process rows are diagnostic detail, not a symmetric
  Carbon-versus-OSF measurement;
- WPA module stacks attribute in-process CPU after a result is reproduced.

## Controlled renderer shootout

Build the small optional UIBench SFSE consumer:

    pwsh -NoProfile -File .\tools\profiling\Build-UIBenchFixture.ps1 -Scenario static -Resolution 1920x1080

This produces build/profiles/ui-bench/fixture-mod.zip. Install it once in MO2
as UIBench and enable it in all three benchmark profiles. Keeping the same
consumer plugin in the baseline makes its tiny resident load part of every
condition. It registers the OSF UI view or creates the Carbon UI view hidden
after that framework is ready, then presents it when the benchmark save's
`LoadingMenu` closes. With neither framework present it stays inert.

The archive owns one canonical UIBench/fixture/index.html. The setup script
copies those exact bytes into both framework content paths and records the
SHA-256 in both URLs/configuration. Configure an installed fixture while
Starfield is stopped:

    pwsh -NoProfile -File .\tools\profiling\Set-UIBenchFixture.ps1 -ModPath 'C:\Modding\Starfield\MO2\mods\UIBench' -Scenario static -Resolution 2560x1440

Restart Starfield after changing resolution. Load the benchmark save; the
fixture opens automatically after that load completes. OSF UI's authoring viewport is
fixed by its manifest, while Carbon's zero-sized public-API view follows the
backbuffer; the capture preflight requires both JavaScript viewport dimensions
to equal -Resolution. The fixture is deliberately passive so input/focus work
does not contaminate renderer measurements. Re-run `Set-UIBenchFixture.ps1`
while Starfield is stopped to select one of these scenarios:

| Key | Scenario | Pressure applied |
|---:|---|---|
| 1 | static | No per-frame visual mutation |
| 2 | transforms | 192 independently transformed composited tiles |
| 3 | repaint | Full-viewport CSS gradient repaint |
| 4 | layout | Width changes plus forced layout reads over 520 rows |
| 5 | text-scroll | Deterministic scroll through 260 text paragraphs |
| 6 | canvas | Full-resolution Canvas 2D redraw with 420 primitives |

The identity panel displays the document's once-per-second RAF callback rate;
this is page update cadence, not Starfield application FPS or proof that every
callback reached the display. The page also writes the same cadence in one
telemetry report per second through each framework's public bridge to
`Documents\My Games\Starfield\SFSE\Logs\UIBench.telemetry.jsonl`. A controlled capture is rejected unless the live report has the
expected process ID, framework, scenario, viewport, recent timestamp, and
64-character fixture hash. The reports copied into each capture contain RAF
cadence, long-frame counts, work-function percentiles, and long tasks.

Generate the default 1440p/60 three-workload screening list:

    pwsh -NoProfile -File .\tools\profiling\New-UIBenchMatrix.ps1

The default matrix is nine matched Baseline/OSF UI/Carbon UI triplets, or 27
individual captures, and keeps WPR disabled. Pass all three resolutions, all
three rate modes, and all six scenarios explicitly only after the screening
result justifies the exhaustive 486-capture matrix.

### Resumable automated runner

`Invoke-UIBenchMatrix.ps1` executes a generated matrix and checkpoints its CSV
after every attempt. It can configure the installed fixture, launch the exact
MO2 profile through MO2's command-line runner, restore Starfield focus, warm
the validated scene, capture the row, gracefully close Starfield, cool down,
and generate the final comparison. Failed or interrupted rows are retried when
the same command is run again; completed rows are skipped.

Close the normal MO2 window before using automatic profile launching. For the
most reliable run, load the benchmark save and confirm readiness once per
launch when prompted. `-AutoDetectReady` removes that confirmation, but should
only be used when the load flow itself is deterministic. `-AutoCloseGame`
sends a normal window-close request after each successful capture and never
force-terminates Starfield.

```powershell
pwsh -NoProfile -ExecutionPolicy Bypass -File .\tools\profiling\Invoke-UIBenchMatrix.ps1 `
  -MatrixPath .\build\profiles\ui-bench-real-4k60\matrix.csv `
  -FixtureModPath 'C:\Modding\Starfield\MO2\mods\UIBench' `
  -ModOrganizerPath 'C:\Modding\Starfield\MO2\ModOrganizer.exe' `
  -DurationSeconds 60 -WarmupSeconds 30 -CooldownSeconds 15 `
  -WprProfile None -AutoCloseGame
```

Use `-PlanOnly` to inspect remaining rows and the minimum timed duration
without launching or changing anything. Use `-MaxCaptures 1` for a production
workflow check; that completed row remains checkpointed when the unrestricted
command resumes. Keep WPR disabled for the headline matrix, then reproduce only
an important condition with `-WprProfile CpuGpu` for module/stack attribution.

### Claim rubric

The harness is deliberately falsifiable; it does not assume OSF UI wins. A
defensible statement that OSF UI is more CPU-efficient requires all of:

1. The same fixture hash, viewport, game save/camera, render preset, frame-rate
   mode, Frame Generation state, and repeat IDs.
2. Comparable page RAF cadence. If one engine runs the workload at half the UI
   rate, report that quality/cadence difference beside CPU rather than calling
   the lower CPU number an equal-output win.
3. Lower median baseline-adjusted tracked CPU core-ms/application-frame for OSF
   UI across at least three repeats.
4. No material regression in application FPS, displayed FPS, p99 frame time,
   or 1 percent low.
5. The advantage reproduces in more than one animated workload and its scaling
   is checked at 1080p, 1440p, and 4K.

WPR module attribution should agree with the direction of the process-level
delta. A source-code explanation such as CPU rasterization or a full-surface
upload explains a reproduced result; it is not a substitute for one.

Primary references used by the harness:

- Carbon UI public API and implementation:
  https://github.com/CarbonNode/CarbonUI
- PresentMon console metrics and frame-type semantics:
  https://github.com/GameTechDev/PresentMon/blob/main/README-ConsoleApplication.md
- Microsoft xperf sampled CPU module attribution:
  https://learn.microsoft.com/windows-hardware/test/wpt/profile-wta

## One-time MO2 setup

Clone the same clean gameplay profile three times in the MO2 UI. Keep every mod
and INI identical except for these framework switches:

| Profile | OSF UI | Carbon UI |
|---|---:|---:|
| `UI Bench - Baseline` | disabled | disabled |
| `UI Bench - OSFUI` | enabled | disabled |
| `UI Bench - CarbonUI` | disabled | enabled |

Enable the same UIBench fixture mod in all three profiles. Do not leave the
framework's own F10/F7 panel open during a controlled fixture capture.

Do not benchmark with both enabled. `Capture-UIBench.ps1` inspects the loaded
Starfield modules and rejects the run unless the selected state is true. For a
Carbon run it requires both `CarbonUI.dll` and `CarbonUICore.dll`, which also
catches a loader that started but failed to initialize the framework core.
It also checks `-Resolution` against Starfield's actual client area before
creating a capture.

Use the same save, camera, resolution, render scale, frame cap, upscaler,
quality preset, Frame Generation state, and background applications. Warm the
save before the countdown. Starfield pauses on focus loss, so start the command
from an elevated PowerShell 7 terminal and restore game focus during the
countdown.

### Rasterization policy

`-RasterizationPolicy FrameworkDefault` is the default and answers the
end-to-end product question. It records each framework's effective raster
without requiring them to match. The comparison report labels any mismatch;
do not interpret a mismatched condition as equal-pixel renderer efficiency.

`-RasterizationPolicy PixelMatched` is the controlled renderer mode. It
requires devicePixelRatio 1.0 and equal effective raster sizes. OSF UI now
tracks Starfield's native output size rather than capping its browser surface
at 1440p, so the current OSF UI and Carbon UI fixtures can both rasterize at
3840x2160. Keep rasterization policy identical across all runs in a condition.

## Comparison capture

Use at least three repeats for every framework and scenario. The metadata values
are comparison keys, so spell `RenderPreset` identically across all runs.

```powershell
pwsh -NoProfile -ExecutionPolicy Bypass -File .\tools\profiling\Capture-UIBench.ps1 `
  -Framework Baseline -Scenario loaded-hidden -Repeat 1 `
  -Resolution 2560x1440 -FrameGeneration Off `
  -FrameRateMode Fixed60 `
  -RenderPreset Ultra-DLSSQuality -DurationSeconds 60

pwsh -NoProfile -ExecutionPolicy Bypass -File .\tools\profiling\Capture-UIBench.ps1 `
  -Framework OSFUI -Scenario loaded-hidden -Repeat 1 `
  -Resolution 2560x1440 -FrameGeneration Off `
  -FrameRateMode Fixed60 `
  -RenderPreset Ultra-DLSSQuality -DurationSeconds 60

pwsh -NoProfile -ExecutionPolicy Bypass -File .\tools\profiling\Capture-UIBench.ps1 `
  -Framework CarbonUI -Scenario loaded-hidden -Repeat 1 `
  -Resolution 2560x1440 -FrameGeneration Off `
  -FrameRateMode Fixed60 `
  -RenderPreset Ultra-DLSSQuality -DurationSeconds 60
```

Each framework change requires a separate Starfield launch from the matching
MO2 profile. An interleaved run order reduces thermal/time bias: Baseline → OSF
→ Carbon for repeat 1, Carbon → OSF → Baseline for repeat 2, then OSF →
Baseline → Carbon for repeat 3.

Captures are stored below
`build/profiles/ui-bench/runs/<framework>/<scenario>/`. A repeated identity is
safe: the comparison keeps the newest completed capture for an identical
framework/scenario/repeat combination and reports a warning.

Generate the comparison after the run matrix is complete:

```powershell
pwsh -NoProfile -File .\tools\profiling\Compare-UIBench.ps1
```

The outputs are `build/profiles/ui-bench/comparison.md` and
`comparison.json`. Optional filters include `-Scenario`, `-Resolution`,
`-FrameGeneration`, `-FrameRateMode`, `-RenderPreset`, and
`-RasterizationPolicy`. The report warns about missing
frameworks, mismatched repeat IDs, absent PresentMon data, or fewer than three
runs; override the last threshold with `-MinimumRepeats` only for a deliberate
smoke test.

## Comparison scenarios

| ID | State | Valid conclusion |
|---|---|---|
| `loaded-hidden` | Fresh launch; framework loaded; no framework UI opened | Resident framework cost versus baseline |
| `warm-hidden` | Open the framework UI once, close it, then capture | Post-initialization/cache cost versus baseline |
| `shipped-visible` | OSF F10 settings or Carbon F7 demo remains visible | Real shipped default-stack cost; documents differ |
| `shipped-interaction` | Repeat the same timed pointer/scroll sequence | End-to-end interaction and hitch behavior; documents differ |
| `open-close-soak` | Repeat open/close cycles for 30+ minutes | Memory, VRAM, handle, and thread drift |
| `static` through `canvas` | Identical UIBench document at a validated CSS viewport; effective raster recorded separately | FrameworkDefault: end-to-end product comparison. PixelMatched: controlled renderer/compositor comparison |

Baseline has no framework UI, so for visible/interaction scenarios it measures
the same game scene without an overlay. The shipped-visible comparison must not
be presented as isolated WebView2-versus-Ultralight renderer efficiency because
OSF's settings document and Carbon's demo document have different content.

## Profiler coverage

The profiler captures the boundaries OSF UI actually owns:

- `Game`: Starfield plus the in-process `OSFUI.dll` D3D12 compositor and runtime.
- `OSFUIHost`: `osfui_webview2_host.exe`, including WGC capture and the D3D11
  `CopyResource` into the shared texture ring.
- `WebView2`: only `msedgewebview2.exe` descendants of that OSF UI host.

The sampler records normalized CPU, working set, private bytes, handles,
threads, I/O rates, per-process GPU engine occupancy, dedicated/shared GPU
memory, system pressure, and NVIDIA power/clock/temperature data when
`nvidia-smi` is available. WPR adds symbolizable CPU stacks and GPU queue
activity. A PresentMon console binary is optional and adds frame-time, GPU-time,
and latency percentiles. For Carbon UI, all framework modules appear in `Game`.

## FPS, normalization, and attribution

PresentMon is launched with frame-type tracking. Summaries keep application
FPS/1% low separate from displayed FPS and generated-frame rows. A Fixed60 or
Fixed120 capture is excluded from comparison if measured application FPS is
outside plus or minus 5 percent, with a 3 FPS minimum tolerance. Uncapped is
recorded as a condition rather than treated as a target. The script records
the monitor's current display mode, but it cannot set a game, driver, or RTSS
frame cap for you.

When WPR is enabled, xperf automatically exports
cpu-profile-by-module.txt plus a UI-module-focused extract. WPA is still the
right tool for time-range and full-stack analysis.

Derived CPU values use the sampled one-core-normalized process CPU:

    core-ms/s = mean CPU percent of one core x 10
    core-ms/application-frame = core-ms/s / application FPS

The comparison subtracts the matched baseline in core-ms/s first, then divides
by that framework's application FPS. It also divides by viewport megapixels.
That order avoids disguising different frame rates as renderer efficiency.

Carbon's stock public API does not expose renderer-render/upload counters.
The common page therefore reports those fields as unavailable instead of
guessing. The report may show width x height x 4 as theoretical visible BGRA
surface volume; it is explicitly not a measured memory-bandwidth number.

## Single OSF UI capture

Run from an elevated PowerShell 7 terminal. Establish a reproducible save,
camera position, resolution, render scale, frame cap, and Frame Generation
state before comparing captures.

```powershell
pwsh -NoProfile -ExecutionPolicy Bypass -File .\tools\profiling\Capture-OSFUI.ps1 `
  -Label loaded-never-opened -DurationSeconds 60
```

The five-second countdown lets you return focus to Starfield. Captures are
written to `build/profiles/osfui/<timestamp>-<label>/`; `summary.md` and
`summary.json` are generated automatically.

For PresentMon, put an official console binary on `PATH`, place it beneath
`external/presentmon`, or pass `-PresentMonPath`. The profiler still works
without it; inspect present scheduling in the WPR ETL instead. The summarizer
accepts both legacy and current CSV column sets. The older copy bundled with
NVIDIA FrameView SDK is deliberately not auto-selected because it fails these
combined capture arguments without producing a CSV.

PresentMon captures only the selected Starfield PID in parallel with the WPR
session. Post-processing a generic WPR GPU ETL is not a substitute: it lacks
the process attribution PresentMon needs and can emit monitor-level events as
an unknown process.

Use a short CPU/GPU trace for a hitch or steady-state workload:

```powershell
.\tools\profiling\Capture-OSFUI.ps1 `
  -Label settings-visible-1440p -DurationSeconds 60 -WprProfile CpuGpu
```

Use low-overhead sampling for a lifecycle/memory soak:

```powershell
.\tools\profiling\Capture-OSFUI.ps1 `
  -Label open-close-soak -DurationSeconds 1800 -IntervalSeconds 5 -WprProfile None
```

`General` adds first-level triage providers when file I/O, hard faults, waits,
or unrelated system activity may explain a hitch:

```powershell
.\tools\profiling\Capture-OSFUI.ps1 `
  -Label first-open-hitch -DurationSeconds 30 -WprProfile General -OpenWpa
```

## OSF UI deep-dive matrix

Capture each steady state for 60 seconds at least three times and use the
median run. Warm the save and page once before recording unless first-open
behavior is the subject of the capture.

| ID | OSF UI state | What it isolates |
|---|---|---|
| A0 | OSF UI disabled for a separate otherwise-matched launch | True game-only baseline |
| A1 | OSF UI loaded; no view has been opened | Resident startup host/browser cost while hidden |
| A2 | Open once, then close all views | Post-warm hidden-state and browser-cache cost |
| A3 | Static focused menu visible | Normal WebView2 composition, WGC copy, and D3D12 blend |
| A4 | Representative animated/interactive page | JS/layout/paint pressure and capture cadence |
| A5 | Passive HUD visible during the same gameplay path | Continuous non-capturing presentation cost |
| A6 | Same as A3 with restart-latched 240 Hz capture enabled | High-refresh policy delta |

Repeat A1-A5 at 1920x1080, 2560x1440, and 3840x2160 if those resolutions are
supported. Repeat the important pair with Frame Generation off and on. Change
one variable at a time; do not compare different saves or camera paths.

For lifecycle stability, capture at least 100 open/close cycles or 30 minutes.
Judge private bytes, dedicated GPU memory, handles, and threads after returning
to the same hidden state. Browser caches may rise initially; a suspected leak
should remain monotonic across repeated cycles and fail to plateau.

## Reading the evidence

The generated summary answers the broad questions quickly:

- CPU: mean, p95, and peak normalized machine utilization by role.
- RAM: working-set and private-byte maxima and start-to-end deltas.
- VRAM: dedicated/shared process GPU memory and whole-adapter usage.
- GPU work: 3D and Copy engine occupancy by role, plus power, clocks, and
  temperature where supported.
- Stability: handle, thread, private-memory, and VRAM drift.
- Frame pacing: p50/p95/p99 frame time, approximate 1% low FPS, CPU/GPU busy
  time, and display latency when PresentMon is available.

Open `trace.etl` in WPA for attribution:

1. In **CPU Usage (Sampled)**, group by `Process > Module > Stack`. Filter
   Starfield to `OSFUI.dll` to separate native OSF UI samples from the rest of
   the game. Load the PDBs using the symbol path printed after capture.
2. In **GPU Usage**, inspect GPU Hardware Queue and per-process engine work.
   `OSFUIHost` Copy-engine work corresponds to the WGC/D3D11 transport. The
   fullscreen D3D12 blend is recorded inside Starfield, so use the A0/A1/A2
   scenario delta as well as the queue events.
3. For `General` captures, correlate CPU spikes with disk/file I/O, hard
   faults, context switches, and wait chains before assigning the hitch to OSF
   UI.
4. Inspect the PresentMon CSV/summary for p95 and p99 regression even when
   average FPS is unchanged. Average utilization alone can hide frame spikes.

Do not choose an optimization from a single run. A result is actionable when
the same stack, GPU queue, or resource slope repeats and the candidate scenario
regresses against its matched baseline. If the external trace cannot separate
the winning hotspot, the next step is narrow opt-in instrumentation: D3D11 and
D3D12 timestamp queries around the copy/blend, plus QPC spans and counters for
renderer update, bridge pumping, published/consumed frames, and dropped waits.
