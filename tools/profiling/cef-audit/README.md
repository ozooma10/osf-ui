# CEF performance attribution

This is a desktop diagnostic, not a production backend change. It never deploys
or launches Starfield. `prepare.py` copies the current CEF helper into
`build/cef-audit/source`, checks every patch anchor, and adds instrumentation
there. The original sources, scheduler and runtime stay intact.

The hooks follow the event-driven scheduler and shared 60 Hz publication deadline.
Historical polling/message/event experiments retain their generated sources and
binaries in their original runs. Use `Compare-Backends.ps1` for uninstrumented
comparisons. Close Starfield before either runner; game contention invalidates a run.

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
| `baseline` | Stage timings only; current production scheduler |
| `reuse` | Same timings, with Chromium's same-site renderer process reuse switch |
| `copyonly` | During measured intervals, retain CEF paint copies but suppress output composition/publication |
| `discard` | During measured intervals, count CEF paint callbacks without touching their borrowed textures; suppress output |
| `webview2` | Original WebView2 helper as a contemporaneous reference |

`copyonly` and `discard` are deliberately nonfunctional output controls. They
still run the same page, JavaScript and CEF off-screen rendering. The runner
explicitly passes `--allow-no-output true` to the consumer, which labels the
result and does not save stale screenshots. These modes estimate the cost of
removing integration work, not a usable or validated rendering optimization.

## Measurements and limits

`-Cases baseline,fullcopy` compares damage copying with a forced full-copy
control in the same diagnostic build. `copy_fraction` and `copy_mib` record
actual submitted regions; normal production contains no diagnostic switch.
Use uninstrumented old/new helpers for throughput claims, because diagnostics
can change scheduling. Mixed cases validate all three layer markers and the
shrinking HUD's erased pixels as well as recording each layer's freshness.

- `audit.json` contains per-workload wall-time distributions for queue residence,
  message handling, borrowed-texture open/copy/fence wait, paint callbacks,
  composition/publication, frame cadence and ring backpressure. Nested metrics
  overlap and must not be added together. The fence metric includes signal,
  flush, scheduling and wait; it is not pure GPU copy duration.
- `-GpuQueries` adds pooled D3D11 timestamp/disjoint queries around our own copy
  and compositor commands. Resolution is asynchronous with `DONOTFLUSH`; no
  extra GPU wait is introduced. These runs are a separate diagnostic group.
- `-Retention` measures three animated views, hides them, then restores them.
  The consumer retains process-memory snapshots at each stage. The instrumented
  host records `owned_paint_mib`: live copied view/popup texture pixel storage
  (width × height × 4), excluding the output ring, driver overhead and Chromium's
  own resources. It is not a measure of total VRAM or process-private commitment.
  Retention runs are separate from the six-workload performance comparison.
- The consumer records total and root-process CPU, plus per-process cumulative
  CPU, private commitment and working-set snapshots. CPU 100% means one logical core. The CEF root contains OSF code
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

The borrowed CEF texture lifetime and copy-completion wait remain intact in all
functional variants.

`write_report.py` renders the historical polling-versus-event experiment only;
use `summarize.py` for current audit data.
