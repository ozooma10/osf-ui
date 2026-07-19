# Renderer comparison — 2026-07-19 (first instrumented A/B)

Same dual-backend build (plugin + `Bench:` instrumentation identical), same
machine, same view (`osfui/settings`), same output 3440x1409, back-to-back
sessions. Runs: `20260719-112428-webview2` (out-of-process host) and
`20260719-113419-ultralight`.

**Caveats — treat as indicative, not final.** Both sessions were far shorter
than the protocol (overlay open ~17 s vs ~8 s, instead of 3+ min), the
overlay-closed baselines were different scenes (Starfield GPU 81% vs 57%
closed — so game-side GPU/CPU deltas are NOT attributable to the renderer),
and n=1 run each. The game-thread/present-hook channels are still solid
(tens of thousands of samples), and the memory/process-footprint numbers are
stable within their windows.

## Overlay open, steady state

| metric | webview2 (out-of-proc host) | ultralight (in-proc) |
|---|---|---|
| game-thread tick cost avg (ms) | **0.001** | 0.004 |
| present-hook cost avg / p99 (ms) | 0.054 / 0.123 | **0.046** / 0.109 |
| in-process frame production | none (GPU shared textures) | 0.538 ms avg, p99 12.4, max 209 (worker thread) |
| web frames produced /s | 1.9 | 5.7 |
| compositor consumes /s | 2.0 | 3.8 |
| extra processes | 7 (host + Chromium) | 0 |
| external tree CPU (machine %) | 1.91 | 0 |
| external tree memory (WS / private MB) | 395 / 428 | 0 |
| combined CPU, game + tree (machine %) | 8.26 | **6.45** |
| combined working set (MB) | **8451** | 8513 |
| combined private (MB) | 10354 | 10407 |
| orphan processes after exit | 0 | 0 |

## Reading

- **Both backends are effectively free on the game's threads.** Tick cost is
  1–4 µs; the present-hook composite is ~50 µs per present for both
  transports. Nothing here can produce visible frame-rate impact.
- **The real difference is WHERE the web engine's cost lands.**
  - Ultralight paints on an in-process worker: ~0.5 ms average per pump but
    with a heavy tail (p99 12 ms, max ~209 ms during load/first paint).
    Those spikes are on the worker thread, not the game thread — but they
    are inside the game process (scheduler competition) and its memory.
  - The WebView2 host moves all of it out of process: ~400 MB and ~2% CPU
    in 7 external processes, zero in-process production cost, GPU-to-GPU
    frame transport.
- **Total memory is a wash** (~8.4–8.5 GB combined working set either way):
  Ultralight inflates Starfield's own working set by roughly what the
  WebView2 tree costs externally.
- **Total CPU while open favored Ultralight** in this pass (6.5% vs 8.3%
  machine) — Chromium's fixed per-process overhead outweighs Ultralight's
  paint cost on a mostly-static settings view. A JS/animation-heavy view
  would likely tilt the other way; that scenario is unmeasured.
- Ultralight repainted ~3x more often on identical content (5.7 vs 1.9
  frames/s) — worth a look at what dirties the view (caret? CSS anim?).
- Both exited clean: zero orphaned processes.

## What a rigorous pass needs

1. Full protocol length (3+ min open, 1 min closed baseline) — same save,
   same spot, both runs.
2. A worst-case content run (animation/scroll-heavy view) to measure the
   backends under load, not at idle.
3. 2–3 repeats per backend for variance.
