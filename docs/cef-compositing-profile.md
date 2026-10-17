# CEF compositing profile — October 17, 2026

Validation is in progress. The implementation builds and passes the native and
bridge suites. The final repeated desktop comparison and packaged in-game runs
still need an uninterrupted desktop with other Starfield sessions closed.

## Attribution

The current event-driven helper was profiled separately from the production
performance runs on a Ryzen 9 7950X / RTX 3080 at 2560×1440. Diagnostic code is
generated under `build/cef-audit`; it is not compiled into shipped helpers.

With three animated views, each browser produced about 60 paint callbacks/s, and
the helper published about 60 composites/s. The independent paint and output
cadences sometimes sampled the top view twice, reducing distinct output frames.
Two fresh, uninstrumented six-second baseline trials delivered 47.7–49.7 new
frames/s. Instrumentation itself changed delivery, so its throughput must not be
used as the baseline.

A separate GPU-query run measured mean copy execution of 0.077 ms and composition
of 0.159 ms. The copy's signal/flush/completion path took 0.717 ms of wall time on
average. These overlapping measurements cannot be added together. Most browser
CPU was outside the host process. Removing the borrowed-texture completion wait
would violate CEF's resource lifetime contract; it remains intact.

## Changes

- Coalesce recently animating layers for at most four milliseconds after the
  shared output deadline, preserving the next 60 Hz deadline and immediate bridge
  scheduling. Static views cannot hold up animation indefinitely. Two exploratory
  production trials delivered 55.6–59.3 distinct frames/s with three views; the
  final repeated comparison remains pending.
- Stop hidden browsers painting, ignore their late callbacks, and release copied
  view/popup textures. Instrumented five-cycle hide/show testing recorded 42.19 MiB
  of owned paint pixels while three 1440p views were visible and zero when hidden,
  returning to 42.19 MiB after every show. This excludes the output ring, driver
  overhead and Chromium allocations. It is not a process RAM saving: private
  commitment remained high and varied across reopen cycles.
- Retry a reveal refresh once after 34 ms if no post-reveal paint arrived.
  Callback tracing caught a static view painting before its reveal sentinel,
  then receiving no additional callback from the immediate refresh. The retry
  checks document token, visibility, loading and close state; pre-reveal pixels
  are never accepted as a fallback.
- Expand real-helper tests to verify transparent overlap, z-order, hide/reshow,
  destruction of the top layer, and lower-layer animation under static overlays.
  Failed tests retain pixels, an epoch and a screenshot where available.

Same-site renderer reuse saved approximately 75–85 MiB in the three-view
experiment but did not consistently improve delivery. It is not enabled in
production because it would couple renderer-thread responsiveness across views.
Chromium describes the [process reuse tradeoff](https://chromium.googlesource.com/chromium/src/+/main/docs/process_model_and_site_isolation.md).

## Evidence and remaining work

Local artifacts are retained under `build/cef-optimization/`:

- `before-audit-local/` and `before-report/`: original production and timed helper.
- `reuse-audit/`: same-site process reuse experiment.
- `coalesced-performance/`: exploratory uninstrumented optimized runs.
- `gpu-profile/`: GPU timestamps, separate from throughput comparisons.
- `retention-cycles/`: original package versus instrumented optimization, five
  hide/show cycles and per-process memory snapshots.
- `layers-baseline/`: both original helpers passed all layer checks available then.
- `lifecycle-final/`, `reshow-repro/`, `reshow-trace/`: retained static-reveal failure
  and diagnostic callback ordering.
- `reveal-fix-lifecycle/cef-1440p-r1/`: clean pass of all 15 helper lifecycle checks
  after the refresh fix. The next repetition was invalidated by another Starfield
  session starting; it is not counted as final acceptance.
- `native-tests.log`, `web-tests.log`: all 28 registered native tests and nine
  bridge/compatibility tests passed; the updated frame-pacer test also passed.

Pending: repeated clean renderer/lifecycle runs, final performance comparison of
both backends, exact-package in-game acceptance, and final validation manifests.
No publication is authorized or performed. Desktop output rates are not game FPS;
private commitment is not resident RAM or VRAM.
