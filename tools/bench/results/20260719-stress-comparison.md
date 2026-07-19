# WebView2 (out-of-process host) vs Ultralight — animation-heavy content

2026-07-19. Same dual-backend build, same machine, same view
(`osfui.bench/stress`), same output 1280x768, back-to-back sessions of the
same protocol. Runs: `20260719-123830-webview2-stress` and
`20260719-124440-ultralight-stress`. Overlay open ~245 s each, two full
six-scene cycles per run.

The earlier `settings`-view pair (`20260719-comparison.md`) answered "what
does an idle view cost". This answers "what happens when the UI moves", and
the answer is categorically different.

## Per scene — what the web engine delivered

| scene | webview2 fps | ultralight fps | webview2 p95 frame | ultralight p95 frame | webview2 long frames | ultralight long frames |
|---|---|---|---|---|---|---|
| idle | 120.0 | 43.6 | 8.4 ms | 33 ms | 0 | 82 |
| css-transform | 119.8 | 30.3 | 8.4 ms | 36 ms | 1 | 407 |
| **paint** | **119.3** | **13.9** | 8.4 ms | **79.5 ms** | 1 | 557 |
| canvas | 120.0 | 30.8 | 8.4 ms | 37 ms | 0 | 604 |
| layout | 106.9 | 33.4 | 16.7 ms | 34 ms | 2 | 74 |
| fullscreen | 120.0 | 46.8 | 8.4 ms | 23 ms | 0 | 1 |

WebView2 holds the 120 Hz display ceiling on five of six scenes. Ultralight
does not reach 50 fps on any scene and collapses to 13.9 fps on `paint`
(gradient + shadow + blur invalidation), where every single frame it managed
was a long frame.

`layout` is the one scene where they converge (106.9 vs 33.4 — the narrowest
ratio at 3.2x): DOM reflow is main-thread work in both engines, so Chromium's
GPU compositing advantage does not apply. `fullscreen` is Ultralight's best
scene (46.8 fps, 1 long frame) because one big uniform gradient is the
cheapest thing a CPU rasterizer can do — no per-element invalidation.

## Cost to the game

| metric | webview2 | ultralight |
|---|---|---|
| main-thread tick avg (open) | **0.001 ms** | 0.013 ms |
| main-thread tick p99 | 0.02 ms | 0.433 ms |
| present-hook avg / p95 | **0.055** / 0.099 ms | 0.059 / 0.189 ms |
| in-process frame production | none (GPU shared texture) | **11.221 ms avg**, p95 12.5, max 78.5 |
| frames delivered to screen /s | **39.9** | 25.7 |

Both are negligible on the game's critical threads — 13x on tick is still
13 microseconds. The decisive number is `produce`: Ultralight spends
**11.2 ms of CPU per worker pump** inside the game process. That is the whole
story of the fps table. It is off the game thread, so it does not stall
rendering, but it is one saturated CPU worker inside Starfield, and it caps
the UI at ~26 delivered frames/s.

## Resource footprint (overlay open)

| metric | webview2 | ultralight |
|---|---|---|
| Starfield CPU (of machine) | 9.07% | 11.25% |
| external tree CPU | 4.58% | 0 |
| **combined CPU** | 13.65% | **11.25%** |
| external processes | 8 | 0 |
| Starfield working set | 8165 MB | 8283 MB |
| external tree working set | 556 MB | 0 |
| **combined working set** | 8720 MB | **8283 MB** |
| orphans after exit | 0 | 0 |

Read naively this favors Ultralight: 2.4 points less CPU, 437 MB less memory.
But it is doing far less work. Normalized per delivered frame:

- webview2: 13.65% / 39.9 fps = **0.34 %-machine per frame**
- ultralight: 11.25% / 25.7 fps = **0.44 %-machine per frame**

WebView2 is ~22% more CPU-efficient per frame delivered AND delivers 55% more
frames. Ultralight's lower total is a throughput ceiling, not efficiency.

## Verdict

For animated content, WebView2 wins decisively — 3–9x the rendered frame
rate at lower CPU cost per frame, with the work isolated in another process.
For static content the earlier pair showed the opposite (Ultralight cheaper
by ~1.8 points of machine CPU, memory a wash), because Chromium's fixed
multi-process overhead dominates when nothing is repainting.

The deciding question is therefore what OSF UI's views actually do. Static
settings lists: Ultralight is fine and cheaper. Anything with transitions,
animated HUDs, canvas, or scrolling: Ultralight's CPU rasterizer is the
bottleneck and the difference is visible, not academic.

## Open items

1. **48 fps transport ceiling — ROOT-CAUSED to WGC visual-capture cadence
   (2026-07-19, same day).** Ruled out in order: host publish cost is
   0.148–0.177 ms/frame with zero `consume lagging` warnings (not the ring,
   fences, or pipe), and the standalone POC — no game, idle GPU, visible
   window, continuously animating stress view — reproduces the identical
   ceiling: `capture cadence: avg 20.82 ms (48.0/s), min 16.32, max 23.92`
   (cadence instrumentation now in the host, logged every 600 frames). So
   the cap is intrinsic to `GraphicsCaptureItem.CreateFromVisual` over the
   WebView2 composition visual: Chromium renders 120 fps (rAF-verified), but
   DWM commits the offscreen visual at ~21 ms cadence — the min gap sits at
   a 60 Hz beat with roughly every fourth commit missed, independent of GPU
   load or occlusion. Nothing downstream can go faster than this input.
   Practical impact is modest — the stress view's motion is time-based, so
   content still samples correctly at 48 Hz — but any "smooth 60+" ambition
   needs a different frame source, not host tuning. Candidates if pursued:
   a DWM/DComp commit-clock investigation, or the CEF backend's
   OnAcceleratedPaint (per-Chromium-frame shared textures, no DWM in the
   loop — see osf-ui-cef-backend).
2. **GPU attribution is missing.** The WebView2 tree reports 0% GPU
   utilization and 0 MB dedicated GPU memory in every sample, which cannot be
   true for a GPU-composited Chromium. The per-PID GPU counter query does not
   attribute its processes. WebView2's real GPU cost is therefore UNMEASURED,
   and the "wins on cost" claim is CPU/memory only.
3. **Uncontrolled second view.** `osf.animation/browser` registered itself at
   runtime in both runs, so each backend hosted 2 views, not 1. Symmetric,
   but it is load neither run asked for.
4. n=1 per backend, and the sampler's real interval is ~3.3 s (Get-Counter on
   GPU countersets is slow), so external metrics are coarse.
