# CEF performance attribution

This is a desktop diagnostic, not a production backend change. It never deploys
or launches Starfield. `prepare.py` copies the current CEF helper into
`build/cef-audit/source`, checks every patch anchor, and adds instrumentation and
experimental scheduling there. The original sources and runtime stay intact.

The patch anchors describe the pre-October-17 polling helper. Production now has
event-driven scheduling and paced coalescing, so `prepare.py` intentionally fails
on those changed anchors until its diagnostic hooks are ported. Existing audit
runs retain their generated sources and binaries for the historical results.
Use `Compare-Backends.ps1` for the current uninstrumented helpers.

The audit requires the normal CEF host to have been built first. It reuses that
build's Release `libcef_dll_wrapper.lib` and the pinned `external/cef` SDK.

```powershell
python tools/profiling/cef-audit/prepare.py
cmake -S build/cef-audit/source -B build/cef-audit/bin -G 'Visual Studio 18 2026' -A x64 "-DCEF_ROOT=$PWD/external/cef" '-DJSON_INCLUDE=<nlohmann include path>'
cmake --build build/cef-audit/bin --config Release --parallel 4
cmake --build build/backend-bench --config Release --parallel 4
& tools/profiling/cef-audit/Run-CefAudit.ps1 -OutDir build/cef-audit/new-run
python tools/profiling/cef-audit/summarize.py build/cef-audit/new-run --out build/cef-audit/new-report
```

Run the `.ps1` inside PowerShell when passing an array of cases. Each run needs a
new output directory. Profiles, logs, screenshots, helper binary hashes, the
instrumented DLL, generated sources and timing JSON are retained there. Trials
run sequentially and the first case rotates between repetitions.

## Experimental modes

| Mode | Difference from the original helper |
| --- | --- |
| `stock` | Original built CEF helper, no instrumentation |
| `baseline` | Stage timings only; original 16 ms polling/publication |
| `messages` | Reader posts a coalesced UI task to drain messages immediately; publication keeps polling |
| `events` | Immediate message dispatch plus coalesced UI tasks to publish dirty paint output; polling remains as a fallback |
| `copyonly` | During measured intervals, retain CEF paint copies but suppress output composition/publication |
| `discard` | During measured intervals, count CEF paint callbacks without touching their borrowed textures; suppress output |
| `webview2` | Original WebView2 helper as a contemporaneous reference |

`copyonly` and `discard` are deliberately nonfunctional output controls. They
still run the same page, JavaScript and CEF off-screen rendering. The runner
explicitly passes `--allow-no-output true` to the consumer, which labels the
result and does not save stale screenshots. These modes estimate the cost of
removing integration work, not a usable or validated rendering optimization.

## Measurements and limits

- `audit.json` contains per-workload wall-time distributions for queue residence,
  message handling, borrowed-texture open/copy/fence wait, paint callbacks,
  composition/publication, frame cadence and ring backpressure. Nested metrics
  overlap and must not be added together. The fence metric includes signal,
  flush, scheduling and wait; it is not pure GPU copy duration.
- `-GpuQueries` adds pooled D3D11 timestamp/disjoint queries around our own copy
  and compositor commands. Resolution is asynchronous with `DONOTFLUSH`; no
  extra GPU wait is introduced. These runs are a separate diagnostic group.
- The consumer records total and root-process CPU, plus per-process cumulative
  CPU snapshots. CPU 100% means one logical core. The CEF root contains OSF code
  AND Chromium browser-process code. Child CPU contains JavaScript, layout,
  raster, driver and browser infrastructure work, not just CEF framework cost.
- `-Trace` captures Chromium categories through CEF's official tracing API.
  Traces can exceed 100 MiB and materially affect frame rate and CPU. Keep them
  separate from performance trials. `summarize.py --trace <run directory>` joins
  process names and inclusive wall-time scopes to the measured phase boundaries.
  Nested trace scopes must not be summed or treated as CPU samples.
- Phase boundaries travel over the same pipe, so the diagnostic phase window
  differs from the consumer window by the measured scheduling delay. This is
  small relative to six-second trials but should not be hidden.
- The consumer validates actual pixels and synchronously waits for its GPU
  readback; this is not Starfield's D3D12 presentation loop. Three-view workloads
  overlap opaque views and only validate the topmost frame marker.
- GPU engine PDH sums exclude DWM. CEF 150 and WebView2 155 are different Chromium
  versions. Memory is process-tree private commitment, not VRAM or resident RAM.
- A nonzero helper exit now fails the benchmark even if rendering completed.
  Older runs must be checked using `exit_code`, not just their `status` label.

The diagnostic `events` implementation is an experiment without the production
scheduler's shared 60 Hz deadline. The newer production implementation has
separate desktop lifecycle, shutdown, visibility and backpressure checks;
in-game validation remains outstanding.
The borrowed CEF texture lifetime and copy-completion wait remain intact in all
functional variants.
