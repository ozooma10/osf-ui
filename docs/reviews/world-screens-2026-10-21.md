**Remediation completed — 2026-10-21**

All six findings below are fixed. The validated code revision is `4465462d`; the follow-up documentation commit also includes the SDK manifest typings and Browser TV audio correction. The original review is retained below as historical evidence, with its original line references and validation scope.

| Finding | Fix | Commit |
| --- | --- | --- |
| Selected screen loses readiness while another mirror remains healthy | End forwarded/active interaction; retain material restoration metadata when readiness disappears | `532fcab4` |
| WebView2 Stop leaves loading/audio gates unsettled | Track navigation identity and document commit evidence; settle current cancellation and ignore stale callbacks | `f7d4de73` |
| CEF error followed by load-end becomes false success | Retain failed load state until a subsequent document commit | `e9f9647b` |
| Binding tests no longer compile; lifecycle registration incomplete | Move engine audio sampling into `ScreenAudio`; restore independent binding tests; register browser/consent and new reload cases | `532fcab4`, `e9f9647b`, `dc41b140` |
| World views omitted from developer reload | Watch instantiated surfaces, refresh each owning host, pin refresh lifetime, reject old interactions and reconcile input in the same tick | `dc41b140` |
| Browser count represents feed leases | Publish logical bound-reference count through additive surfaces ABI 1.3; label unloaded bindings consistently | `1a1f98c9`, `04d3bef8` |

Implemented performance and simplification work:

- Aggregate binding counts once and batch texture demand under one lock/work scan. Expected demand work is O(bindings + views), replacing repeated registry/surface scans. Repeated material applications with unchanged token/material/UV variant are skipped (`04d3bef8`).
- Move host recovery joins and process waits off the runtime tick, with bounded replay, stale-callback/epoch guards, preserved GPU ownership, and explicit completion. Retirement time does not consume the replacement's response budget. Explicit shutdown retains ownership through callbacks/threads/process exit, preventing last-owner destruction on the transport thread (`4465462d`).
- Scan neutral keys/gamepad only during interaction admission, while continuing active-session cancellation/focus checks (`dc41b140`).
- Coalesce consecutive audio scenes without crossing command barriers or extending their absolute expiry (`b2f0e541`).
- Sleep idle CEF audio workers; retain eager device preparation and active spatial/retry deadlines. Build diagnostics outside the PCM lock and use a typed WebView2 failure accessor (`223abc17`).
- Add typed `fullscreen` and reserved `browser` manifest fields, correct the package ABI requirement and spatial-audio documentation. A separate line-ending correction (`973b33f7`) normalized the selectively staged WebView2 patch without absorbing the existing pipe edits.

These are code-path improvements; no FPS, frame-time, or audio CPU speedup is claimed without live measurements.

**Final validation and deployment**

- Fresh portable native run compiled 69 translation units, linked 37 suites, and passed every executed check, including 19 WebView2 runtime scenarios. Log: `build/fix-final-native.log`.
- Fresh CEF runtime target compiled and passed the same 19 scenarios. Logs: `build/fix-final-cef-lifecycle-build.log` and `build/fix-final-cef-<scenario>.log`.
- Binding suite: 60 checks passed. Navigation: WebView2 125/125, CEF 10/10. Retirement: 23/23. Recovery tracker: 201/201, including a simulated 300-second retirement. Audio queue: 37/37; browser audio: 47/47.
- World asset/view Node tests: 5/5; WebView2 audio-extension tests: 10/10; world-content Python tests: 3/3; browser chrome syntax check passed. SDK typings were checked against parser/schema; no TypeScript compiler was available.
- Production WebView2 core DLL, World Screens DLL, WebView2 helper, and CEF helper built successfully. Core build log: `build/fix-core-recovery-build.log`. Compiler-service C1902/C1090 failures cleared on compiler-access/retry runs without source workarounds. Existing CEF delay-load linker warnings remain.
- One existing symlink escape test was skipped because Windows denied symlink creation. Engine/GPU doubles and synthetic helper barriers do not exercise actual game or browser-process timing.
- With Starfield and browser helpers closed, deployed eight owned files to the configured MO2 core/World Screens folders: core and extension DLL/PDB pairs, WebView2 helper/PDB, browser chrome, and extension README. Every source/deployed SHA-256 matched. Receipt: `build/review-fixes-deployment.json`; backups: `build/review-deploy-backup-20261021-035436`. The configured edition remains WebView2; the CEF helper was built, not deployed.
- The helper build includes the user's existing uncommitted WebView2 pipe changes. Those changes were preserved and excluded from these commits. All other pre-existing dirty files remained byte-identical to the saved baseline. Nothing was pushed, tagged, or published.

No new game/browser session was launched. Fresh Stop/error callback ordering, hung-helper recovery timing, GPU/frame-time measurements, controller input, save/load/unload behavior, audio continuity and endpoint hotplug remain live acceptance work.

**Deferred opportunities**

Shared file-preparation caching, bounded/coalesced browser metadata, and a browser-session extraction remain possible follow-ups; they require additional invalidation/ordering design and measurement. CEF async pipe-writer parity is deferred until the user's separate IPC work is ready to integrate. Dormant binding observation pruning is deferred because those records also retain geometry proof used by later binding. None of these was needed to fix the six confirmed findings.

**Original review snapshot — before remediation**
**OSF UI recent-change review — 2026-10-21**

Reviewed the 30 commits from `33859f80..49d1a0afcffab1b2ea5cb0d77f106d6bb724bba9`, plus the existing working-tree changes to WebView2 IPC and test runners. The emphasis was World Screens: native bindings, material/texture ownership, GPU copies and fullscreen draws, input admission, browser navigation and trust boundaries, both audio backends, content generation, and validation coverage. Findings below describe the reviewed working copy; source line numbers refer to that copy.

The architecture has useful safeguards: the optional extension boundary is clear, shared feeds avoid duplicate browser playback for mirrors, binding handles are session-scoped, GPU retirement retains outstanding reads, fullscreen descriptor heaps belong to their source, and remote content is separated from trusted chrome. Nevertheless, six actionable defects/gaps remain. None of the runtime findings below was reproduced in a fresh game session; they are source-confirmed control-flow or contract defects. The binding-test build failure was reproduced directly.

**Findings, ordered by impact**

1. **[P2] An active interaction survives loss of its selected screen's material/geometry readiness.**

   In [Extension.cpp:67](../../extensions/world-screens/src/Extension.cpp), an interaction already forwarded to core is cancelled only when its token disappears, its view changes, or cancellation is explicitly requested. `selection->ready` is checked only for requests that have not been forwarded. [ScreenBindings.cpp:184](../../extensions/world-screens/src/World/ScreenBindings.cpp) intentionally preserves the logical token/view while invalidating geometry/material evidence during reconstruction; `Resolve` can therefore return the same selection with `ready=false`.

   Trigger: start interacting with one screen, then replace its 3D with an unproven graph or an unavailable material. If another mirror keeps the shared feed healthy, core's feed-level checks still pass. Keyboard/controller capture continues even though the selected binding can no longer present the page. This also affects requests already queued in core but not yet started.

   Cancel a forwarded interaction when its selected binding loses readiness. Keep logical binding persistence independent of permission to hold input. Add a lifecycle test that starts capture, invalidates only that screen, and leaves a second mirror running. The existing binding test already models an unproven replacement graph, but does not connect that case to active interaction.

2. **[P2] WebView2 Stop leaves the current navigation marked as loading.**

   [BrowserSurface.inl:111](../../tools/webview2_host/BrowserSurface.inl) implements Stop with `web->Stop()`. Navigation start sets `browserLoading=true`, `browserLoadSucceeded=false`, and the audio loading gate in [HostApp.cpp:681](../../tools/webview2_host/HostApp.cpp). The completion handler at line 1123 returns immediately for every `OPERATION_CANCELED`, before distinguishing a replaced navigation from cancellation of the current navigation. The normal completion cleanup is skipped.

   Trigger: stop a slow navigation before it completes. The toolbar remains in its loading/Stop state instead of offering Reload. Initial audio capture also waits on `!loading`, and subsequent audio recovery waits on `browserLoadSucceeded`. An already active capture can continue; this is not a claim that Stop always silences playback. A later successful navigation recovers the state.

   Check navigation identity first, then settle an intentional cancellation of the current navigation without treating it as a host-recovery failure. Explicitly choose whether a committed partial document remains usable and update audio/navigation state consistently. Add current-stop and superseded-navigation cases to the host state-machine tests.

3. **[P2] CEF can replace a real navigation failure with a false success.**

   In [Host.cpp:917](../../tools/cef_host/Host.cpp), remote `OnLoadEnd` derives `loaded` solely from whether the URL is allowed. `OnLoadError` at line 928 may already have reported a transport failure. The pinned CEF SDK's `external/cef/include/cef_load_handler.h:102` explicitly permits post-commit errors to occur in combination with `OnLoadStart`/`OnLoadEnd`.

   Trigger: a main document commits, then its connection fails. Error handling emits failure; the following load-end callback accepts the still-valid HTTP(S) URL and emits success. Core clears the browser error guide and treats the partial document as successfully loaded.

   Preserve a failure flag associated with the current navigation until the next navigation starts. Keep completed HTTP error pages browsable, while distinguishing them from incomplete responses and transport failures. Add an error-then-end callback-order test; URI-parser tests cannot cover this transition.

4. **[P2] The screen-binding test target no longer compiles after audio integration.**

   Reproduced with `xmake build -y osfui-screen-binding-tests`: MSVC reports `C1083: Cannot open include file: 'OSFUI_SurfaceAudio.h'`. [xmake.lua:191](../../xmake.lua) supplies only the extension and test-stub include roots, while `ScreenBindings.h` now includes the SDK audio header. The test directly includes the production implementation, which now also depends on camera, scene-graph, preference, and browser-audio facilities absent from its old test setup.

   Updating one include path is necessary but does not establish that the rest of the test harness is repaired. Prefer moving camera/audio snapshot collection into a small engine adapter, leaving binding selection/restoration independently testable; alternatively update the complete stub/include contract. Until the target builds and runs, the latest binding code lacks the advertised private-geometry and session-reset regression coverage.

   Related coverage gap: XMake registers only 13 lifecycle scenarios at [xmake.lua:341](../../xmake.lua). The five newer browser-surface/disabled/consent scenarios are included in `tests/native/run.sh` but omitted from the normal XMake test matrix. Register the same scenario set for both backends.

5. **[P2] Developer live reload skips instantiated world views.**

   [RuntimeViews.cpp:180](../../src/Views/RuntimeViews.cpp) watches only mods with views instantiated in `m_presentation`. World views are instantiated by `SurfaceService`, so a world-only mod never enters the watcher set. Even if the mod also has an overlay view, the reload loop at line 194 again filters out world views and navigates only the overlay renderer. This conflicts with the general live-reload behavior described in `docs/authoring.md:441`.

   Trigger: enable developer mode, load a world screen, then save its HTML/JS/CSS. Its host continues serving the previous document/mirror until it is retired/restarted. Include surface hosts in watcher membership and route refresh/navigation to the owning host, with the same request cancellation and controller-grant reset used for page replacement. Do not simply send world manifests to the overlay renderer.

6. **[P3] The browser's screen count measures leases rather than screens.**

   [SurfaceService.cpp:42](../../src/Surfaces/SurfaceService.cpp) exposes `e.demand` as `screens`, but [WorldTexture.cpp:620](../../extensions/world-screens/src/World/WorldTexture.cpp) acquires just one feed lease per view, regardless of the number of bound references. `chrome.js:119` displays that value as "On N screens", and line 408 describes screens on the channel.

   Two or more TVs sharing `osfui.browser/tv` therefore still report one screen. The current test checks a single synthetic lease and cannot detect the mismatch. Publish an explicit count of bound or loaded screen references, with clearly chosen semantics, and notify chrome when it changes. Audio emitter count is not an equivalent substitute because it is range-filtered and capped.

**Performance and simplification opportunities**

These are source-backed opportunities, not measured speedups. Profile before and after changing scheduling or GPU synchronization.

| Priority | Opportunity | Evidence and proposed direction |
| --- | --- | --- |
| High | Aggregate demand once per tick | `Extension.cpp:60` calls `HasBindings` for every view; each call scans the registry. Each `WorldTexture::SetDemand` then calls `RefreshWork`, which scans surfaces again. Worst-case work is proportional to views × bindings plus views squared. Build one demand/count snapshot and update all surfaces in one pass. This can also supply the correct screen count. |
| High | Keep host shutdown off the runtime tick | `SurfaceService.cpp:277` calls `RestartAfterFailure` synchronously. That reaches `BrowserHostClient.cpp:998` and can wait six seconds for a helper to exit, then one more after termination, in addition to thread joins. A hung helper can stall the game tick; several failed feeds amplify it. Reuse the asynchronous retirement pattern with an explicit restart state, retaining GPU/read ownership throughout. This blocking mechanism also exists in overlay recovery. |
| Medium | Check neutral input only during admission | `RuntimeInput.cpp:201` calls `KeysNeutral` whenever an interaction is pending or active. `OverlayInputHook.cpp:388` scans 255 virtual keys, although an already-started interaction ignores the neutral argument. Keep cancellation/menu/focus checks active, but stop neutral-key and duplicate gamepad sampling after admission. |
| Medium | Share file preparation across surface hosts | `BrowserHostClient.cpp:437` serializes preparation behind a process-wide mutex. Every host walks the shared mod caches again; developer mode copies the entire views tree into every instance mirror at line 468. Share an immutable preparation result per session or prepare only the owner mod plus shared assets. Preserve isolation and the previous fix preventing one host from deleting another's mirror. |
| Medium | Coalesce browser metadata and serialize once | Source/title/history/audio callbacks publish complete browser state. Both hosts serialize for comparison and then again for transport; core republishes it to chrome. The notification queue at `BrowserHostClient.cpp:298` is unbounded. Keep a latest pending metadata snapshot per view and bound fields; preserve ordered command results, failures, and frame ownership messages. No oversized-title exploit was established in this review. |
| Medium | Reuse the asynchronous pipe writer in CEF | The uncommitted WebView2 writer removes pipe writes from its UI thread. `cef_host/Host.cpp:366` still performs synchronous writes from browser/UI callbacks. Apply the same bounded writer/teardown contract after checking shutdown, frame acknowledgments, and failure delivery. This is a backend-parity improvement, not a newly introduced CEF defect. |
| Medium | Let idle CEF audio workers sleep | `cef_host/SpatialAudio.cpp:177` wakes every five milliseconds even when a stream is inactive. Preserve eager device preparation, then wait for a state change or the next real retry/spatial deadline. Two reserved audio routes currently mean two such workers. |
| Medium | Coalesce superseded audio poses | `BrowserBoundedQueue.h:15` has no audio-scene coalescing key, so delayed queues retain obsolete poses. Coalesce consecutive scene snapshots while preserving command barriers and their original absolute expiry; never refresh a stale scene's lifetime. |
| Low | Avoid diagnostic allocation in normal audio work | CEF `SpatialAudio.cpp:119` constructs a JSON array in the spatial update loop and periodically builds a diagnostic object even outside developer mode. WebView2 `AudioRouting.inl:7` constructs a status object just to test whether capture failed. Use typed status accessors and construct diagnostic snapshots only when consumed. |

For maintainability, separate three concerns currently mixed in `SurfaceService`: generic surface demand/host lifetime, exclusive interaction, and the reserved browser's navigation/guide/audio state. A small browser-session object would reduce hardcoded browser branches without introducing a general framework. Likewise, keep the engine camera reader outside the binding registry; the broken test target is direct evidence of the coupling cost. Preserve the existing single-owner scheduling and resource lifetime rules during either extraction.

The binding registry also keeps observations for never-bound and restored references. Its 4096-entry cap can be consumed by dormant observations across a long session. Consider pruning entries that have no logical binding and no material restoration obligation, while retaining the deliberate unload/reload persistence of live bindings. The actual frequency of reaching this cap was not measured.

Add `fullscreen` and the reserved `browser` fields to `sdk/osfui.d.ts` so the SDK matches the parser/schema. The current forward-compatible index signature accepts them as `unknown`, losing useful autocomplete and validation.

Two documentation-only mismatches also need correction: `tools/package-world-screens.ps1:49` records surfaces ABI 1.0 in the archive manifest while the extension requires 1.2; `extensions/browser-tv/README.md:28` still says audio is not spatial. Update these from the current contract and keep dated acceptance evidence distinct from implementation capability.

**Validation and limits**

- Fresh `tests/native/run.sh`: compiled 66 translation units, linked 34 suites, and passed every executed check, including all 18 lifecycle scenarios. This is the script's default WebView2 build with engine/GPU doubles, not a second CEF-native build. Log: `build/review-native-20261021.log`.
- One existing CEF asset-test symlink escape case was skipped because the execution account could not create the symlink.
- WebView2 audio extension Node tests: 10/10 passed.
- Dirty pipe-writer changes: forced `xmake build -r wv2-pipe-tests` passed; the freshly rebuilt executable passed 175 checks with zero failures. One nonblocking shadowed-local warning remains in `wv2_pipe_tests.cpp:249`.
- World Screens Node asset/view tests: 5/5 passed.
- World content Python tests: 3/3 passed.
- Browser chrome JavaScript syntax check passed; `git diff --check` passed.
- Screen-binding target: failed to compile as described above. The initial sandbox/compiler-access errors were retried with compiler access; the missing header is the substantive source/configuration failure.
- Sandbox named-pipe access restrictions also required the pipe test to run with approved access; that rerun passed. This was an execution-environment restriction, not a product failure.
- Full production DLL/helper builds, package generation, fresh GPU/gameplay checks, physical controller input, endpoint hotplug, and a new two-TV save/load/unload session were not performed. Unit success does not establish those results.

The earlier six World Screens fixes remain present, including per-source descriptor heaps, instance-isolated developer mirrors, scoped controller resets, and malformed-feed isolation. This review found no demonstrated bypass of the trusted chrome/native bridge boundary. It does not close the already documented browser release gates in `extensions/world-screens/docs/BROWSER_SURFACE.md:543`, including non-HTTP/private-network interception limits, file-picker behavior, and print preview. The initial remote-controller readiness versus chrome's one-shot resume also deserves an asynchronous integration test; problematic runtime ordering was not established here.

Recommended order: repair the binding test seam, fix selected-binding invalidation and both navigation terminal-state defects, add their focused regressions to both backend matrices, then address reload/count correctness. Take demand aggregation and off-thread recovery as separate measured performance changes. No production code was modified by this review.
