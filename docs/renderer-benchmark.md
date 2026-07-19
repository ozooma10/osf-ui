# Renderer benchmark — Ultralight vs WebView2

Objective, repeatable comparison of the two web renderer backends on
performance and resource usage. First run: 2026-07-19.

## What is measured

Two instrument layers, produced by the same binary and scripts for both
backends so numbers are directly comparable.

### Internal (`benchStats` config knob → `Bench:` log lines)

`src/core/BenchStats.{h,cpp}`; enabled by `"benchStats": true` in
`config.json`, otherwise every probe is a single relaxed atomic load.
Windows flush every ~5 s and on every overlay visibility edge, so no window
mixes overlay-open and overlay-closed samples. Channels (ms, with
n / avg / p50 / p95 / p99 / max per window):

| channel | thread | meaning |
|---|---|---|
| `frame` | main | frame-to-frame delta of the SFSE per-frame task = game frame time |
| `tick` | main | overlay cost inside `Runtime::Tick` (`renderer->Update` + `SubmitFrameIfVisible`) — the only overlay work the game thread pays besides the present hook |
| `present` | present hook | overlay-attributable cost per drawn present: texture/upload management, fence waits, record + submit (`D3D12Compositor::OnPresent` below the visibility gate) |
| `produce` | renderer worker | in-process CPU cost to obtain one web frame — **semantics differ by design**: Ultralight = one full worker pump (engine `Update` + CPU paint + harvest + publish, idle pumps included); `webview2-inproc` = one WGC readback (`CopyResource` + `Map` + memcpy); `webview2` (out-of-process host) = NO in-process cost at all — frames arrive as GPU shared textures, so the channel is empty and only the produced-frame count ticks; the host's cost appears in the external sampler |

Plus a `rates` line: web frames produced/s and compositor uploads/s (the
effective on-screen UI frame rate).

### External (`tools/bench/Sample-OsfUiBench.ps1`)

Out-of-process sampler, ~1 Hz CSV until the game exits: Starfield CPU %
(normalized to the whole machine), working set, private bytes, GPU engine
utilization, dedicated GPU memory — and the same columns summed over the
OSF UI external tree: `osfui_webview2_host.exe` plus every OSF UI-owned
`msedgewebview2.exe` (matched by `OSFUI` in the command line; zero for
Ultralight, whose entire cost is in-process).
On game exit it snapshots `OSF UI.log` next to the CSV and appends an
orphaned-WebView2-process count.

`tools/bench/Report-OsfUiBench.ps1 -Csv <csv> -Log <log>` renders one run
into a markdown summary (internal channels split by overlay state, external
columns split into closed/open phases via the log's visibility edges).

## Test content

Two subjects, because a renderer comparison on one page is a comparison of
that page, not of the renderers.

**`osfui/settings`** — the shipped view: mostly static, repaints rarely. The
idle/typical-use case.

**`osfui.bench/stress`** (`tools/bench/view/`, installed only for benchmark
runs — deliberately NOT in `data/OSFUI`, so it never ships) — six scenes,
cycled 20 s each, that isolate different pipeline stages:

| scene | what it stresses | why it discriminates |
|---|---|---|
| `idle` | nothing | floor cost of a loaded view |
| `css-transform` | compositing | 150 keyframe transform/opacity nodes: a GPU browser re-composites without repainting; a CPU renderer must repaint every frame |
| `paint` | rasterization | 60 tiles whose gradient/shadow/blur change per frame — no compositor shortcut for either backend |
| `canvas` | raster + JS | 1400-particle Canvas2D with a fixed per-frame draw budget |
| `layout` | layout engine | 240 rows re-measured per frame (widths + text metrics) |
| `fullscreen` | transport | whole viewport rewritten per frame; defeats dirty-rect optimization |

Determinism is enforced in the view: seeded PRNG for all placement (no
`Math.random`), motion is a pure function of elapsed scene time (a slower
renderer samples the same animation less often rather than running a
different one), and scene time advances only while the overlay is visible so
both runs align to overlay-open time. The view reports its own throughput
per scene — with `benchStats` on, the runtime mirrors page console output
into the log as `Bench: [<viewId>] [stress] scene=… avgFps=… p95=… ` lines,
which is the WEB ENGINE's account of the same window the native channels
measure from the game's side.

Keys while it runs: `1`–`6` hold a scene, `0` resume cycling, `P` pause,
`R` reset stats.

Three traps this view hit, worth knowing before authoring another one:

- **A `nativeBridge: true` view MUST load `shared/osfui.js`** (or install
  `osfui.onMessage` itself). Otherwise the runtime holds its queued messages
  and keeps it permanently off screen — F10 shows nothing, with the reason in
  `OSF UI.log`. Ultralight logs the warning; symptoms differ per backend.
- **The backends disagree about rAF while hidden.** Chromium suspends it for
  a hidden view; Ultralight keeps calling it. A benchmark view must gate its
  own animation on `ui.visibility` rather than trusting rAF to stop, or one
  backend silently runs the workload during the closed-overlay baseline. The
  runtime only sends `ui.visibility` on an EDGE, so assume hidden at boot
  whenever the bridge exists.
- **Do no DOM work on the hidden path** — not even a stats HUD. On Ultralight
  those writes dirty the surface and charge repaints to the baseline window
  that WebView2 never pays.

## Running a pass

```powershell
xmake                                                  # build + deploy first
tools\bench\Prepare-BenchRun.ps1 -Renderer webview2   -View stress
#   ... launch via MO2, play the session, quit ...
tools\bench\Prepare-BenchRun.ps1 -Renderer ultralight -View stress
#   ... same session again ...
tools\bench\Report-OsfUiBench.ps1 -Csv results\<stamp>-<label>.csv -Log results\<stamp>-<label>.log
```

`Prepare-BenchRun.ps1` writes the deployed config, installs/removes the
stress view, warns if the deployed DLL isn't the freshly built one, and arms
the sampler.

## Fairness rules

- One dual-backend build (`xmake f -m releasedbg --with_ultralight=true
  --with_webview2=true`): the ONLY difference between runs is the
  `renderer` string in the deployed `config.json`. (This build combination
  required the xmake.lua on_load merge — see the comment there.)
- Same machine state: no other 3D apps, same MO2 profile, same save, same
  display/resolution, game kept foreground the whole run (it pauses on
  focus loss).
- Same session shape per backend (timings approximate; the log records the
  real edges):
  1. launch via MO2 → main menu → load the same save
  2. ≥60 s idle, overlay closed (baseline)
  3. F10 → overlay open, hands off, ≥240 s — two full scene cycles when
     running the stress view (steady state)
  4. Esc → ≥30 s closed → F10 → ≥60 s open (reopen cost / stability)
  5. Esc → quit to desktop normally (orphan check)
- Renderer ids: `webview2` is the OUT-OF-PROCESS host (GPU shared-texture
  transport; no MO2 workaround needed); `webview2-inproc` is the legacy
  in-process backend, which still requires `msedgewebview2.exe` on MO2's
  executables blacklist (Settings → Workarounds) or the browser process
  dies at launch with 0x8000FFFF (see webview2-spike-report.md).
- The MO2 profile must run the DEV mod (`+OSF UI`), not `OSF UI DIST` —
  the DIST package is a stale Ultralight-only DLL with no instrumentation.
  Verify `Bench: stats enabled` appears in the first seconds of
  `OSF UI.log` before investing a session.
- Same display resolution/window mode in both runs: output size sets the
  per-frame pixel cost. The log's `Runtime: output WxH` records it — check
  the two runs agree before comparing them.
- Other mods that call `RegisterView` (e.g. OSF Animation) add a second
  hosted view to BOTH runs. Symmetric, so not fatal, but it is uncontrolled
  load — disable them for a clean pass and note it either way.

## Known asymmetries (read before quoting numbers)

- `produce` is per-pump for Ultralight vs per-frame for WebView2 — compare
  it together with the `rates` line and the external CPU columns, not in
  isolation.
- WebView2's Chromium renders on its own processes (and GPU); Ultralight
  paints on the in-process worker CPU. The honest totals are the combined
  rows in the report (Starfield + WebView2 tree).
- WebView2 is single-view in this phase; the config's `views` list is
  capped to the default view. Ultralight runs the same single view in this
  protocol, so the comparison holds, but multi-view cost is out of scope.
- The capture path readback ring adds ~2 captured frames of latency on
  WebView2 (spike report); interaction latency is not measured here.

## Results

Run artifacts land in `tools/bench/results/` (`<stamp>-<renderer>.csv`,
`.log`, `.md`). Comparison write-ups go in this file's git history.
