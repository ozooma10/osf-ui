# OSF UI performance profiling

This kit measures OSF UI without adding profiler code to the shipping DLL. It
captures the costs at the boundaries OSF UI actually owns:

- `Game`: Starfield plus the in-process `OSFUI.dll` D3D12 compositor and runtime.
- `OSFUIHost`: `osfui_webview2_host.exe`, including WGC capture and the D3D11
  `CopyResource` into the shared texture ring.
- `WebView2`: only `msedgewebview2.exe` descendants of that OSF UI host.

The sampler records normalized CPU, working set, private bytes, handles,
threads, I/O rates, per-process GPU engine occupancy, dedicated/shared GPU
memory, system pressure, and NVIDIA power/clock/temperature data when
`nvidia-smi` is available. WPR adds symbolizable CPU stacks and GPU queue
activity. A PresentMon console binary is optional and adds frame-time, GPU-time, and latency
percentiles.

## Capture a scenario

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

## Required scenario matrix

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
