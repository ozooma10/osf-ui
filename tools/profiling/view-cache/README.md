# View cache preparation benchmark

Builds the real `ViewCache.cpp` in a separate executable. It does not launch the
game, deploy files, or touch normal browser caches. Run while other builds and
game/browser benchmarks are stopped.

```powershell
cmake -S tools/profiling/view-cache -B build/view-cache-bench -G 'Visual Studio 18 2026' -A x64
cmake --build build/view-cache-bench --config Release --parallel 4
& build/view-cache-bench/Release/osfui_view_cache_bench.exe build/cache-new-run
```

The output directory must not exist. Synthetic cases contain 1, 8, 32 and 128
mod folders, each with HTML, a manifest and a 64 KiB asset. They cover initial
copying, seven metadata-cache hits, one changed mod, and stale-entry cleanup.
An optional second argument selects an existing source tree to **read only**;
that mode measures initial preparation and seven cache hits. All cache writes
and retained fixtures stay in the output directory.

Use `-DCACHE_SOURCE=<saved-ViewCache.cpp>` to compare an exact previous
implementation. Keep the header and driver identical. Hash sources/executables,
alternate baseline/candidate order, and use identical source trees. Do not compare
first-copy and warm samples or silently replace failed trials.

`-DPROFILE_IDENTITY=ON` instruments a generated copy in the build directory and
counts/times filesystem identity calls. Production source and binaries receive
no instrumentation. Compare total times with this option **off**; use the
instrumented build for attribution. Identity timing is nested inside elapsed
preparation time and must not be added to it. CSV also records Windows process
I/O operation/byte deltas; metadata operations appear among `other_ops`.

Initial copy failures are retained in `failures.txt`. Bounded preparation retries
are labeled `partial-retry`; they are never empty-cache measurements. Changed-mod
failures are retained too. Warm failures stop the run. This retry mechanism belongs
only to the diagnostic fixture; it does not change production recovery behavior.
Inspect failures alongside CSV before interpreting results.

These measurements use ordinary NTFS paths, not MO2's live VFS. OS file caches
are not purged. They measure elapsed preparation work, not game-thread stalls,
browser first-paint time, rendered FPS, allocations, or long-session stability.
