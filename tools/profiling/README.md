# Profiling

Measure OSF UI from outside the shipping DLL. `Capture-OSFUI.ps1` samples three roles and writes `summary.md` and `summary.json` to `build/profiles/osfui/<timestamp>-<label>/`.

| Role | Processes |
| --- | --- |
| `Game` | Starfield with the in-process `OSFUI.dll` compositor and runtime |
| `OSFUIHost` | `osfui_webview2_host.exe`: WGC capture and the D3D11 copy into the shared texture ring |
| `WebView2` | `msedgewebview2.exe` descendants of that host |

Sampled: CPU, working set, private bytes, handles, threads, I/O, per-process GPU engine occupancy, GPU memory, and NVIDIA power/clock/temperature when `nvidia-smi` exists. WPR adds CPU stacks and GPU queue activity. PresentMon (optional) adds frame-time, GPU-time, and latency percentiles.

## Capture

Run from an elevated PowerShell 7. Fix the save, camera, resolution, render scale, frame cap, and Frame Generation before comparing runs. A five-second countdown lets you refocus Starfield.

```powershell
# Steady state or hitch, CPU+GPU stacks
.\tools\profiling\Capture-OSFUI.ps1 -Label webview-visible-1440p -DurationSeconds 60 -WprProfile CpuGpu

# Lifecycle / memory soak, low overhead
.\tools\profiling\Capture-OSFUI.ps1 -Label open-close-soak -DurationSeconds 1800 -IntervalSeconds 5 -WprProfile None

# First-level triage: file I/O, hard faults, waits
.\tools\profiling\Capture-OSFUI.ps1 -Label first-open-hitch -DurationSeconds 30 -WprProfile General -OpenWpa
```

PresentMon: put an official console binary on `PATH`, under `external/presentmon`, or pass `-PresentMonPath`. It captures only the Starfield PID alongside WPR. The FrameView SDK copy is not auto-selected; it fails these arguments. Without PresentMon, read present scheduling from the WPR ETL.

## Scenarios

Capture each for 60 seconds, three times, and use the median. Warm the save and page once unless first-open is the subject.

| ID | State | Isolates |
| --- | --- | --- |
| A0 | OSF UI disabled, matched launch | Game-only baseline |
| A1 | Loaded, no view opened | Resident hidden cost |
| A2 | Opened once, then closed | Post-warm hidden and cache cost |
| A3 | Static focused menu | WebView2 composition, WGC copy, D3D12 blend |
| A4 | Animated/interactive page | JS/layout/paint and capture cadence |
| A5 | Passive HUD during gameplay | Non-capturing presentation cost |
| A6 | A3 with 240 Hz capture enabled | High-refresh policy delta |

Repeat A1-A5 at 1080p, 1440p, and 4K, and the key pair with Frame Generation off and on. Change one variable at a time. For lifecycle, run 100 open/close cycles or 30 minutes and judge private bytes, dedicated GPU memory, handles, and threads back in the same hidden state; a leak stays monotonic and never plateaus.

## Read the results

The summary reports CPU (mean/p95/peak by role), RAM and VRAM maxima and drift, GPU engine occupancy, handle/thread drift, and PresentMon frame pacing (p50/p95/p99, 1% low, CPU/GPU busy, latency).

In WPA with `trace.etl`:

1. **CPU Usage (Sampled)**: group by Process > Module > Stack; filter Starfield to `OSFUI.dll`. Load PDBs from the symbol path printed after capture.
2. **GPU Usage**: `OSFUIHost` Copy-engine work is the WGC/D3D11 transport. The D3D12 blend runs inside Starfield, so use the A0/A1/A2 delta.
3. `General` captures: rule out disk I/O, hard faults, context switches, and waits before blaming OSF UI.
4. Check p95/p99 in the PresentMon summary even when average FPS is flat.

Act only on a hotspot or slope that repeats across runs and regresses against its matched baseline. If external traces cannot separate it, add narrow opt-in instrumentation: D3D11/D3D12 timestamp queries around copy and blend, plus QPC spans for renderer update, bridge pumping, frame publish/consume, and dropped waits.
