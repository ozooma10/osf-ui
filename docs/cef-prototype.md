# Experimental CEF backend

OSF UI can run its existing HTML views through a bundled CEF helper. WebView2 remains the default. The CEF path is a prototype: it compiles and passes focused desktop tests, but rendering, input, GPU compatibility, and shutdown have not yet been verified in Starfield.

## Enable or revert

Install a build containing `SFSE/Plugins/OSF/UI/bin/cef/` alongside the normal OSF UI plugin. In OSF Settings, open **OSF UI → Developer → Use experimental CEF renderer**, enable it, then restart Starfield. The setting key is `cefPrototype`; it is read once at startup. Turn it off and restart to return to WebView2. There is no automatic backend fallback.

OSF Settings itself does not depend on this browser backend, so the toggle remains accessible when the selected browser fails. The CEF runtime must be installed as a complete folder, including the helper EXE/DLL, `libcef.dll`, supporting DLLs, `.pak` files, locales, and notices. An arbitrary `libcef.dll` is not a replacement for this matched distribution.

## Build

The prototype uses the official Windows x64 minimal distribution pinned in [`cef.lock.json`](../tools/cef_host/cef.lock.json): CEF `150.0.22+gc1a097e+chromium-150.0.7871.256`. Setup verifies the archive's SHA-256. Only the small helper and CEF C++ wrapper are compiled; Chromium comes prebuilt.

In addition to the normal OSF UI build prerequisites, install CMake 3.25 or newer and Visual Studio 2026 C++ tools. From the repository root:

```powershell
pwsh tools/setup-cef.ps1
xmake f -P . -m releasedbg --cef=y
xmake build -P . 'OSF UI'
```

The CEF target invokes `tools/build-cef-host.ps1` and stages the complete payload in `build/cef-host/runtime`. Normal XMake deployment also copies it to the configured mod folder. Builds without CEF use `--cef=n`; this does not change a player's saved backend setting or remove an existing CEF installation. The helper script accepts `-Generator` for another compatible CMake Visual Studio generator and `-JsonInclude` for a nonstandard nlohmann_json 3.11.3 installation.

An explicitly requested prototype archive can be built with:

```powershell
pwsh tools/package.ps1 -IncludeCefPrototype
```

That archive has `-cef-prototype` in its name and still defaults to WebView2 at runtime. Normal packaging explicitly disables CEF. With `-SkipBuild`, the configured payload must match the requested package type. Packaging has not been exercised for this prototype yet.

## Implementation

- The game continues using the existing browser-host protocol, view lifecycle, bridge, recovery, and D3D12 compositor. `WebView2HostWebRenderer` retains its historical name but selects either helper at startup. Consumer HTML, C++, and Papyrus APIs do not change.
- CEF's sandbox-enabled bootstrap EXE loads `osfui_cef_host.dll`. Renderer and GPU subprocesses use that same distribution. The helper creates windowless browsers with transparent accelerated rendering and sends input through CEF's keyboard, mouse, and IME APIs.
- Each accelerated callback opens the borrowed texture, copies its visible rectangle into a helper-owned D3D11 surface, and waits for that copy to complete before returning. The helper composites view/popup surfaces into the existing shared-texture ring. Game frame acknowledgments prevent premature ring reuse. No CEF-owned handle is sent to Starfield.
- Pipe arrivals wake a coalesced CEF UI task immediately; each task handles at most 128 messages before yielding. Dirty visible surfaces share one pending publication and one 60 Hz deadline for the complete view stack. A one-shot high-resolution Windows timer wakes the CEF UI thread only when a dirty frame has a future deadline. New paints replace the pending surfaces instead of queuing redundant composites. Hidden paint callbacks do not request output.
- A full frame ring suspends publication until an acknowledgment arrives; resizing can replace the blocked ring. Static views have no publication polling. Browser-close callbacks drive shutdown, with a separate five-second timeout; parent-process checks and heartbeats run only at the protocol heartbeat cadence. Texture copying and composition remain on the CEF UI thread.
- Local HTTPS URLs retain the `osfui.example` origin. File requests resolve inside the prepared view tree; traversal, Windows alternate streams, and canonical path escapes are rejected. Native bridge injection is limited to the owning main document. The current SDK and the existing 1.x compatibility transport are embedded unchanged where applicable.
- Network requests outside the local host, page-created windows, downloads, and file dialogs are blocked. Existing network guards also disable worker/socket APIs. Developer mode permits an explicitly opened DevTools window.
- MO2's virtual runtime folder is copied to a real, versioned `%LOCALAPPDATA%/OSFUI/cef-runtime/gen-*` directory before launch. Unchanged copies are reused. Old generations are retained to avoid deleting files still held by Chromium children; automatic cache pruning is not implemented.
- CEF has a separate `%LOCALAPPDATA%/OSFUI/CEF` profile. Logs are `OSF UI.cef-host.log` and `OSF UI.cef-host.log.chromium.log` in the normal Starfield SFSE logs directory. The plugin log identifies the selected backend and runtime version.

## Limits and acceptance

The staged runtime is about **410 MiB**, plus its materialized runtime copy and browser profile. CEF updates must be shipped deliberately with matching helper/SDK binaries and notices; this backend does not inherit Evergreen updates.

Only accelerated BGRA8 rendering is implemented. A software paint callback reports an explicit failure. The helper opens textures on Starfield's adapter; CEF choosing a different GPU may fail. The synchronous texture-copy wait is intentionally conservative and needs profiling. IME composition is forwarded, but candidate-window placement and accessibility are not implemented. No claim of better reliability or performance is supported until live testing.

Desktop checks:

```powershell
xmake test -j4 'osfui-cef-assets-tests/*' 'osfui-cef-frame-pacer-tests/*' 'osfui-runtime-lifecycle-tests/*' 'osfui-settings-client-tests/*'
node --test tests/native/cef_bridge_tests.cjs
```

These cover local-file containment, frame deadline coalescing and stall recovery, default/CEF configuration through the real Runtime, and both JavaScript SDKs against the injected bridge. A symlink containment check explicitly reports a skip on systems that deny symlink creation. They do not start CEF or Starfield and do not prove native callback ordering or GPU synchronization.

A separate [desktop backend benchmark](../tools/profiling/backend-bench/README.md) now exercises both actual helpers, their JavaScript bridges, accelerated shared textures, and normal exit. The October 16 comparison completed twelve sessions at 1080p/1440p using WebView2 155 and CEF 150. It favored WebView2 on median CPU, committed memory, bridge latency, and new-frame delivery; some 1440p runs varied substantially. Raw measurements and the HTML report are under `build/backend-comparison/comparison-20261016/` in the development checkout. This is desktop helper evidence; Starfield's compositor and live input acceptance remain separate.

The October 17 event-driven scheduler also passes the benchmark's explicit `-Lifecycle` scenario: full-ring backpressure, responsive bridge traffic while blocked, acknowledgment-driven delivery of the latest static pixels, hide/show, a message burst exceeding one drain batch, resizing a blocked ring, destroy/reopen, and normal shutdown. Evidence and exact binary/source hashes are under `build/cef-scheduling/lifecycle-verified/`.

The completed October 17 comparison is under `build/backend-comparison/event-driven-20261017-verified/`: twelve successful sessions, three repetitions per backend/resolution, with Starfield closed throughout. CEF's animated bridge RTT p95 medians are 1.14–1.54 ms, compared with 19–24 ms in the earlier polling runs; WebView2's current medians are 0.39–0.67 ms. Single-view CEF output is about 60 distinct FPS versus 56.4 for WebView2. Three-view CEF publication is paced near 60/s, but distinct top-view delivery is only 53.0 FPS at 1080p and 51.3 at 1440p, versus 56.4 for WebView2. The 1080p CEF repetitions range from 45.9 to 54.5 FPS. Multi-view pacing remains an optimization target; capped publication does not establish 60 distinct frames per view.

CEF still uses more process-tree CPU and private committed memory in these workloads. For example, the three-view 1440p medians are 48.6% of one logical CPU and 677 MiB for CEF, versus 33.7% and 518 MiB for WebView2. These measurements describe desktop helpers, not game FPS. Earlier partial batches marked `interrupted.json` or `contention.json` are excluded. Full validation and deployment records are in `build/cef-scheduling/validation.json`.

Before broader distribution, manually verify an existing menu and HUD with CEF selected: transparency and overlapping views, show/hide/reopen, mouse drag/double-click/wheel and selects, keyboard/controller navigation, IME, Alt-Tab, resolution changes, load transitions, browser recovery, and normal game exit. Verify a legacy view and both message/state APIs, then disable the toggle and confirm WebView2 still works. Include a machine without WebView2 and a multi-GPU machine when assessing dependency and adapter behavior.

CEF references: [distribution and embedding](https://chromiumembedded.github.io/cef/general_usage.html), [Windows sandbox bootstrap](https://chromiumembedded.github.io/cef/sandbox_setup.html), and the pinned SDK's `include/cef_render_handler.h` / `include/internal/cef_types_win.h` for accelerated texture ownership.
