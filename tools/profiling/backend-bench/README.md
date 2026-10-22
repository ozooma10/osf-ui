# Browser helper benchmark

This runs the built WebView2 and CEF helpers against the same desktop consumer of host protocol 5. It is independent of Starfield, SFSE, MO2, player settings, and installed browser profiles. It starts real Chromium processes and GPU work; run it deliberately on an otherwise quiet desktop.

Build both helpers first (`xmake build 'OSF UI'` with the WebView2 edition selected, then `xmake build osfui-cef-host`). Then run `pwsh tools/profiling/Compare-Backends.ps1` from the repository root. Defaults: three repetitions, 1920×1080 and 2560×1440, six workloads per session, three seconds warmup and ten seconds measured per workload. Each session receives a new profile. Backend order reverses every other repetition. Output goes to a new `build/backend-comparison/<timestamp>` directory; existing output directories are refused.

Close Starfield normally first. The runner checks before starting and once per second during each session. If the game starts, the current session finishes and is marked with `contention.json`; the comparison stops and the summarizer rejects it. The runner never closes the game. Other background GPU work can also distort results and should be avoided.

The consumer selects the high-performance DXGI adapter and passes its LUID to both helpers. Each completed frame is copied on the GPU into an owned texture. A 32x32 marker is read after a completion fence, before acknowledging the ring slot. Eight RGB blocks encode the full 24-bit serial with binary channels; the decoder rejects the middle third of each channel's range and nonopaque samples. This tolerates both quantization and cross-channel color conversion in WGC without losing serial bits. It verifies nonblank content and counts changed frames rather than duplicated captures. Screenshots are read outside measurement windows. The benchmark's own process and copy cost are excluded from reported browser CPU, but the extra GPU work remains part of this environment. Run the marker and HUD probe checks with `ctest --test-dir build/backend-bench -C Release --output-on-failure`.

Measurements:

- Fresh-process/fresh-profile time to hello, document load, first frame, and first validated content. OS caches are not cleared, so these are not cold-boot startup numbers.
- New output frames per second, delivered messages, frame-gap percentiles, and consumer copy/wait timing. A static page producing zero new frames is expected.
- Native-to-JavaScript echo round-trip latency at 10 Hz. This does not measure mouse/controller input-to-photon latency.
- CPU time summed across the helper and its descendants. 100% means one logical CPU; divide by logical CPU count for whole-machine percentage. Exited discovered processes remain in the CPU accounting.
- Summed process-private committed memory, working set, and process counts, with per-process memory snapshots for attribution. Private commit is not resident RAM or VRAM. Summed working set may count shared pages multiple times.
- Optional Windows GPU Engine counters, summed across descendant processes/engines. This is not a whole-GPU utilization percentage, and excludes DWM and other shared services.

Workloads in `view.html`: static 160-card menu; 700-circle canvas animation targeting 60 updates/s; animated DOM transforms and width changes; three overlapping full-size canvas views. The static page retains a lightweight requestAnimationFrame callback, one stats message per second, and the 10 Hz echo. Browser animation timers and OS composition cadence can limit output below the page's target update rate.

Two mixed cases combine a 60 Hz animated DOM menu, a small transparent HUD at
10 or 30 Hz, and a static translucent overlay. Separate markers expose each
layer through the others. Three binary markers and three HUD probe pixels are read in one GPU completion/map,
with independent freshness and gap statistics in `layers`; the main FPS column
describes the menu. Both animated layers must advance, every marker must remain
valid, and the static marker must remain unchanged. The HUD alternates widths;
its marker determines whether the probe must match an always-filled bar pixel or
an exposed background pixel from the same completed frame. The references must
remain distinct. This detects missing bars and trails from incomplete damage
copies without depending on pre-conversion RGB values. These synthetic cases exercise
sparse painting and mixed cadences; they do not represent a specific content mod.

The runner records binary, fixture and CEF scheduling source hashes, OS/CPU/GPU details, and actual runtime versions. The bundled CEF runtime and installed Evergreen may use different Chromium versions: results compare these shipped implementations, not CEF and WebView2 in the abstract. Developer mode stays off. Each helper and its children belong to a benchmark-owned kill-on-close job; normal pipe shutdown is attempted first and reported. A failed workload stops the run and preserves evidence.

`Summarize-Backends.ps1 -RunDir <path>` requires all expected runs to pass with valid markers and clean exit. It writes `summary.json` and a readable `comparison.html`, showing medians and observed ranges. The output/s column includes repeated top-view markers, while new frames/s counts changes; their difference helps expose redundant composites. A lower layer may legitimately change while the top-view marker stays unchanged. Raw JSON, logs, and PNGs remain available for inspection. No runtime instrumentation is compiled into either backend.

For an explicitly requested scheduler/lifecycle check, run this separately from the performance measurements:

```powershell
& tools/profiling/Compare-Backends.ps1 -SkipBuild -Lifecycle -Backends cef -Repetitions 1 -Heights 1080 -OutDir build/cef-scheduling/lifecycle-check
```

This fills the actual output ring, checks that bridge traffic stays responsive, switches to static content while blocked, and verifies that releasing acknowledgments publishes the pending pixels without a continuing paint stream. It then exercises hide/show, a 300-message burst, resize while blocked, destroy/reopen, and normal shutdown. Three additional static views use black, half-transparent white and half-transparent gray areas. Neutral colors keep the exact alpha-composition checks independent of display gamut conversion; reversing layer order still produces different expected pixels. Pixel reads verify all layers and their overlap, z-order changes, hide/reshow, and destroying the top layer. Screenshots retain the layer test output. Lifecycle results have no performance phases and should not be passed to the performance summarizer.

The results do not establish Starfield FPS, D3D12 compositing cost, MO2 materialization time, disk footprint, long-session stability, or performance of a particular mod's UI. Use the existing in-game capture workflow for those questions.

`-ColdOpen` is a separate first-frame diagnostic: it checks initial startup,
hides the first menu, then creates, reveals and destroys a second menu three
times. It retains the first frame of each presentation epoch and requires that
very first helper frame to be complete and contain the expected pixels. Results
include each first frame's serial, completion flag, marker and center pixel.
The native `--cold-legacy true` option supports a
locally copied legacy fixture under `bench/second` and checks center opacity.
Blank first frames remain failures of this strict probe even if a later frame
contains the page. This desktop consumer does not execute the game's Settings
preparation handshake or `SharedFrameState` queue. Consequently it cannot prove
or disprove the game-side fix that retains the latest frame arriving during that
handshake; use the native queue tests and the packaged cold launcher game check.

`-CefHost <exe>` selects an extracted historical package or diagnostic helper for
controlled comparisons. Metadata identifies that override and hashes the actual
host DLL/runtime beside the executable; source hashes describe the current
checkout, which may differ from an overridden binary. Failed runs retain the last
epoch, sampled layer pixels and a screenshot when the GPU remains available.

## CEF runtime acceptance

After building the CEF helper and this consumer, explicitly run the focused checks
with Node 24 and PowerShell 7 on an otherwise quiet Windows desktop:

```powershell
pwsh tools/build-cef-host.ps1
cmake --build build/backend-bench --config Release --parallel 4
node tools/profiling/Run-CefRuntimeChecks.mjs --out build/cef-runtime/my-run --adapter-indices 0,1
```

Adapter indices use DXGI high-performance ordering. Use `0` on a one-adapter PC;
the runner defaults to `0` and fails if a requested adapter is unavailable. These
checks use the production helper, private profiles, real Chromium processes and
completed GPU texture reads. They do not use the game or installed profiles.

- GPU selection requires ANGLE's reported renderer to match the requested DXGI
  adapter and validates pixels copied from the accelerated CEF texture.
- Storage writes localStorage, IndexedDB and a persistent cookie, immediately
  closes the pipe, and reads them back in a fresh helper process using that profile.
- Media checks HEAD, bounded/open/suffix/clipped ranges, 416, unmatched If-Range,
  and data/blob fetches. A generated 20-second PCM WAV must decode after forward,
  backward and near-end seeks. This does not qualify every video codec.
- View isolation checks fresh healthy-view pixels during a sibling's slow parser,
  pending-message overflow, failed navigation, missed reveal and renderer crash,
  then checks recovery. Partial frames must retain the healthy pixels; complete
  frames must resume when the sibling becomes ready.

Only the crash check enables a temporary loopback debugging port. The runner
targets the private helper's `bench/second` page with `Page.crash`; the consumer
must receive its renderer-termination event and continue receiving main-view
frames. Each consumer owns a kill-on-close job for fallback cleanup, and passing
requires normal helper exit within the production client's six-second deadline.
The runner refuses an existing output directory, stops its own consumer if
Starfield starts, and records source/binary hashes, JSON results, logs and PNGs.
Use `--checks media` (or a comma-separated subset of `gpu,storage,media,isolation`)
for diagnosis, then use a fresh full run to qualify a final helper.
