# CEF optimization handoff — 2026-10-17

Continue on the new Windows PC. The user requested the move before final runtime
testing. Commit focused fixes as work proceeds. Do not push, tag, upload or publish
without a new request. Keep the shared OSFUI.dll/public API and the two automatic
renderer packages; WebView2 remains the recommended default, CEF the bundled
runtime alternative. Do not silently evict hidden browsers or enable process reuse.

## Source state

The working branch on the original PC is `dev` in
`C:\Modding\Starfield\OSF UI`, remote `https://github.com/ozooma10/osf-ui.git`.
The handoff commit contains this document and all pending source changes.

- `575848a`: previous CEF candidate with bounded multi-view coalescing, hidden
  surface release, and a guarded static-reveal refresh retry.
- `e4fe249`: mixed benchmark: 60 Hz menu, 10/30 Hz transparent HUD, static overlay;
  independent markers/freshness for all three layers and report support.
- `aaca46b`: dirty copying. `PaintDamage.h` unions/clips CEF damage in coded
  coordinates; each owned surface retains its own capture-counter history.
  Missing/discontinuous counters, new surfaces, changed geometry or unusable
  damage force a full copy. The borrowed texture is still opened each callback,
  copied into owned storage, and completed before returning to CEF.
- `9ea8260`: `FrameSchedule` owns idle/queued/blocked/stopped publication state;
  `RevealState` owns sentinel/paint/ready phases. Existing timers, bounded
  coalescing, backpressure handling, and token-guarded refresh remain.
- Handoff changes: a fourth readback pixel checks HUD erasure against its marker;
  new headers are included in comparison provenance; diagnostic `fullcopy` forces
  full-frame copies for a controlled comparison. This switch exists only in the
  generated diagnostic helper, never production. Audit deadlines cover six phases.

## What passed, and what did not run

After the production changes: production CEF builds; all 29 registered native
tests and nine bridge/compatibility tests pass. The frame/scheduler/reveal test
has 45 checks and damage planning has 20. The final benchmark and generated
diagnostic helper compile; benchmark JavaScript and runner PowerShell parse.
The updated summarizer successfully processed an existing four-phase dataset.

Before changing production code in this continuation, both old production
helpers passed all 15 real-helper lifecycle checks in three clean 1440p sessions
each: `build/cef-next/baseline-lifecycle/` on the original PC. These results do
NOT validate the subsequent dirty-copy or state changes.

`build/cef-next/baseline-performance/webview2-1440p-r1/contention.json` invalidates
the attempted performance baseline: another Starfield session started. No timing
from that run is accepted. No final CEF mixed workload, HUD erase probe, fullcopy
audit, optimized helper lifecycle or packaged in-game tests have passed yet.
The static refresh, new copy path, and state cleanup all need fresh runtime proof.

No new packages were built after the dirty-copy/state changes, and those changes
were not deployed to the live MO2 mod. Existing candidate archives under
`build/cef-optimization/packages/` describe clean implementation `575848a`, not the
handoff tip. Their two in-game runs were already pending before this continuation.
Earlier successful packaged game tests at `ae48b67` also predate these changes.

Raw local builds, packages, profiles, screenshots, and test artifacts are ignored
and are NOT included in Git/bundles. See `docs/cef-compositing-profile.md` for the
earlier measurements and boundaries. Old-machine timings must not be compared
directly against new-machine timings to claim an improvement.

## Resume on the new PC

1. Import the complete handoff history and inspect status before editing. Read the
   workspace instructions, `README.md`, this file, `docs/browser-backends.md`,
   `tools/profiling/backend-bench/README.md`, and `tools/profiling/cef-audit/README.md`.
   Preserve other branches and dirty work. Do not force-reset the new PC's checkout.
2. Verify the local Windows/MSVC C++23, XMake, CMake, Node, Python, PowerShell 7,
   WebView2 SDK/runtime and pinned CEF prerequisites. Initialize submodules and use
   `tools/setup.ps1` / `tools/setup-cef.ps1` as needed. Adapt local toolchain/path
   configuration to this PC; do not change the CEF pin to bypass setup failures.
3. Rebuild both production helpers and the current benchmark. Obtain an isolated
   baseline CEF build from `575848a` on this same PC (or transfer its verified
   archive). Never overwrite the final helper to obtain a baseline. The comparison
   runner accepts `-CefHost <baseline-exe>` and records actual binary hashes.
4. With Starfield closed, validate the benchmark against both baseline helpers,
   including its new HUD pixel expectation. Then run both current helpers through
   repeated lifecycle and six-workload comparisons at 1080p and 1440p. Use fresh
   evidence directories, retain failures, and reject any game-contended run.
   The HUD must advance at its intended cadence, the static layer must not change,
   and cleared HUD pixels must match the exposed background. Investigate fixture
   assumptions if the new pixel probe fails; it has not yet had live validation.
5. Run the diagnostic baseline/fullcopy comparison, separately from production
   throughput measurements. Inspect `copy_fraction`, `copy_mib`, GPU timings,
   fence wait, per-layer freshness, CPU and private commitment. Do not claim a
   speed or memory improvement merely because less pixel data is copied.
6. Read `tests/game/README.md` and the new PC's sibling OSF Test Harness instructions,
   including `AGENTS.md` and `BACKGROUND.md`. Configure its local disposable save,
   profile, native helpers and private Settings test DLL. The game runner defaults
   to old absolute paths: pass `-HarnessRoot` and `-SettingsTestDll` as appropriate.
   Build `tests/game/Build.ps1`. Finish the previous candidate's pending game tests
   when its exact package is available; clearly label a rebuilt baseline as such.
7. Fix findings, rerun affected checks, and commit. Build both final packages from
   a clean final implementation revision with `tools/package.ps1` into a fresh
   local directory. Run `tests/game/Test-Backend.ps1 -Backend webview2 -Interactive
   -Package <exact-zip>` and the corresponding CEF command. Preserve default soak,
   inspect captures, and bind results to package hashes. Only use harness-owned
   processes and disposable fixtures; never close another game session.
8. Update the profile report with actual outcomes and limitations, commit the
   validation documentation, and leave the tree clean. Do not publish or push.

Useful commands (PowerShell 7, from the repo; choose fresh output directories):

```powershell
xmake test -j4
node --test tests/native/cef_bridge_tests.cjs tests/native/compat_web_tests.cjs
& tools/build-cef-host.ps1
& tools/profiling/Compare-Backends.ps1 -Lifecycle -Repetitions 3 -Heights 1080,1440 -OutDir build/resume/lifecycle
& tools/profiling/Compare-Backends.ps1 -SkipBuild -Repetitions 3 -Heights 1080,1440 -OutDir build/resume/performance
& tools/profiling/Summarize-Backends.ps1 -RunDir build/resume/performance
python tools/profiling/cef-audit/prepare.py
# Configure/build the audit per its README before running:
& tools/profiling/cef-audit/Run-CefAudit.ps1 -Cases baseline,fullcopy -GpuQueries -Repetitions 3 -Height 1440 -OutDir build/resume/copy-audit
```

## Transport without pushing

The original PC creates an incremental Git bundle under `build/handoff/` after
this document is committed. Transfer that file, then fetch the origin repository
on the new PC so the bundle's prerequisite commit is available. Verify the bundle
and import its `refs/heads/dev` into a new local `osf/cef-optimization-handoff`
branch. Do not overwrite an existing branch with that name. The bundle contains
all local `dev` commits after the original PC's `origin/dev`, including the
earlier shared-backend/package work. It contains no ignored runtime or test data.
