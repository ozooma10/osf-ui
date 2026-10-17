# Browser backends

OSF UI has one game DLL and one public HTML/JavaScript, C++, and Papyrus API. Two mutually exclusive packages provide WebView2 or CEF browser hosting. Both use the same view lifecycle, bridge, input policy, recovery, and D3D12 compositor. Browser engines remain in separate helper processes.

**WebView2 is the recommended default for most users. CEF is the bundled-runtime alternative.**

| Edition | Choose it for | Tradeoff |
| --- | --- | --- |
| WebView2 — recommended default | Smaller download and lower measured browser CPU and memory overhead. | Requires Microsoft's WebView2 Evergreen Runtime, which updates independently of OSF UI. |
| CEF — bundled-runtime alternative | A Chromium runtime included and versioned with OSF UI; no WebView2 runtime dependency. | Larger download and higher measured CPU and memory overhead; browser updates must ship with OSF UI. |

This recommendation reflects the current implementations and measured workloads, not a claim that one engine is always faster. CEF delivered more new frames in the single-view animation tests; WebView2 had better median delivery with three animated views. See the measured results below.

Both editions have a [production-package game acceptance suite](../tests/game/README.md) covering rendering, modern and legacy APIs, native/Papyrus messaging, input, save/load, recovery, and shutdown. Results include exact package and helper hashes. Stock-renderer acceptance does not establish every GPU, HDR, physical-controller, or IME configuration.

## Install or switch editions

Close Starfield normally and install **one** edition. With MO2, replace the previous OSF UI installation or disable it before enabling the other edition. Do not enable both editions together: they intentionally own the same plugin and configuration paths. Direct installs should remove the previous edition's browser files under `SFSE/Plugins/OSF/UI/bin` when switching, while preserving consumer views and user data.

Each archive supplies `SFSE/Plugins/OSF/UI/backend.json`:

```json
{ "schemaVersion": 1, "backend": "cef" }
```

The alternative backend value is `webview2`. Selection is read once at startup. The former `cefPrototype` player setting is ignored and no longer appears in OSF Settings. No renderer toggle or separate configuration step is needed. Missing or invalid package selection stops browser initialization with an installation diagnostic; OSF UI does not silently fall back to another engine.

CEF requires its complete matched runtime folder: the bootstrap EXE, host DLL, `libcef.dll`, supporting libraries, resources, locales, lock file, manifest, and notices. The WebView2 edition contains its helper. Both include the same small launcher under `bin/broker`. Browser profiles remain separate, so browser-local storage is not migrated when switching; game/mod-owned data and the public API are unchanged.

## Build and package

Use the normal XMake/MSVC prerequisites. CEF also requires CMake 3.25+, Visual Studio 2026 C++ tools (or an explicitly supplied compatible generator), and the SDK installed by:

```powershell
pwsh tools/setup-cef.ps1
```

The official Windows x64 minimal distribution is pinned in [`cef.lock.json`](../tools/cef_host/cef.lock.json). Setup verifies the download's SHA-256; only the host and CEF C++ wrapper are compiled locally.

For a development build and deployment, select one edition:

```powershell
xmake f -m releasedbg --browser_backend=webview2
xmake build -j4 'OSF UI'
# Or: xmake f -m releasedbg --browser_backend=cef
```

This option changes the host dependency and deployed selection file; it does not add backend compile definitions to `OSFUI.dll`. Development deployment may retain inactive browser files from a previous build; `backend.json` is authoritative. Release archives contain only their selected host payload.

Build both local candidate archives:

```powershell
pwsh tools/package.ps1
# Optional single edition:
pwsh tools/package.ps1 -Backend cef
```

Outputs are `dist/OSF-UI-v<version>-rc-webview2.zip` (recommended default) and `dist/OSF-UI-v<version>-rc-cef.zip` (bundled-runtime alternative), plus SHA-256 and file-manifest sidecars. `-Tag` controls the candidate label; `-Tag ''` removes it. `-SkipBuild` uses existing binaries but still configures the requested edition, stages files, compiles Papyrus, and runs package verification. No packaging invocation deploys, tags, pushes, or publishes. Prepared [release file descriptions](../packaging/nexus/release-2.0-fields.txt) use the same edition labels.

The packager installs the common payload once, then creates each edition from it. It verifies byte-identical common files, actual native API acquisition and compiled Papyrus signatures, selected-backend ownership, the CEF runtime manifest, and every file read back from each ZIP. CEF builds generate a SHA-256 runtime manifest; package verification also checks the bundled lock file against the repository pin.

## Source boundaries

- `src/Runtime`, `src/Views`, `src/Bridge`, `src/Input`, and `src/Composite` own game policy and the public API.
- `src/Render/BrowserBackendConfig` reads the package selection. `BrowserHostClient` owns shared IPC, notifications, demand, and recovery transport. `BrowserHostRuntime` handles backend-specific layout and launch preparation.
- `tools/browser_shared` contains the common protocol, messages, pipe, launch broker, local URI rules, and input data. CDP input remains private to `tools/webview2_host`.
- `tools/browser_broker` is the shared disposable launcher. Under MO2, this process inherits USVFS and asks the desktop broker to launch the selected host outside that environment. Neither browser executable doubles as the injected launcher. In-game testing found that using CEF's sandbox bootstrap for that first injected process crashes before the host handshake. The separate launcher preserves the CEF sandbox and keeps COM launch calls out of Starfield.
- `tools/webview2_host` owns WebView2 controllers/capture; `tools/cef_host` owns CEF callbacks and accelerated off-screen rendering.
- Private host protocol 4 identifies the backend separately from the runtime version and requires shared textures, frame acknowledgments, and presentation epochs. Game DLL and helpers must be installed as one matching package. Public API versions are unchanged.

## Implementation

- The game continues using the existing browser-host protocol, view lifecycle, bridge, recovery, and D3D12 compositor. `BrowserHostClient` is backend-neutral; `BrowserHostRuntime` owns executable layout, profiles, payload checks, and MO2 runtime preparation. Consumer HTML, C++, and Papyrus APIs do not change.
- CEF's sandbox-enabled bootstrap EXE loads `osfui_cef_host.dll`. Renderer and GPU subprocesses use that same distribution. The helper creates windowless browsers with transparent accelerated rendering and sends input through CEF's keyboard, mouse, and IME APIs.
- Each accelerated callback opens the borrowed texture, copies its visible rectangle into a helper-owned D3D11 surface, and waits for that copy to complete before returning. The helper composites view/popup surfaces into the existing shared-texture ring. Game frame acknowledgments prevent premature ring reuse. No CEF-owned handle is sent to Starfield.
- Pipe arrivals wake a coalesced CEF UI task immediately; each task handles at most 128 messages before yielding. Dirty visible surfaces share one pending publication and one 60 Hz deadline for the complete view stack. A one-shot high-resolution Windows timer wakes the CEF UI thread only when a dirty frame has a future deadline. New paints replace the pending surfaces instead of queuing redundant composites. Hidden paint callbacks do not request output.
- A full frame ring suspends publication until an acknowledgment arrives; resizing can replace the blocked ring. Static views have no publication polling. Browser-close callbacks drive shutdown, with a separate five-second timeout; parent-process checks and heartbeats run only at the protocol heartbeat cadence. Texture copying and composition remain on the CEF UI thread.
- Local HTTPS URLs retain the `osfui.example` origin. File requests resolve inside the prepared view tree; traversal, Windows alternate streams, and canonical path escapes are rejected. Native bridge injection is limited to the owning main document. The current SDK and the existing 1.x compatibility transport are embedded unchanged where applicable.
- Network requests outside the local host, page-created windows, downloads, and file dialogs are blocked. Existing network guards also disable worker/socket APIs. Developer mode permits an explicitly opened DevTools window.
- MO2's virtual runtime folder is copied to a real, versioned `%LOCALAPPDATA%/OSFUI/cef-runtime/gen-*` directory before launch. Unchanged copies are reused. Old generations are retained to avoid deleting files still held by Chromium children; automatic cache pruning is not implemented.
- CEF has a separate `%LOCALAPPDATA%/OSFUI/CEF` profile. Logs are `OSF UI.cef-host.log` and `OSF UI.cef-host.log.chromium.log` in the normal Starfield SFSE logs directory. The plugin log identifies the selected backend and runtime version.

## Limits and acceptance

The CEF runtime is about **410 MiB**, plus its materialized runtime copy and browser profile. CEF updates must be shipped deliberately with matching helper/SDK binaries and notices; this backend does not inherit Evergreen updates.

Only accelerated BGRA8 rendering is implemented. A software paint callback reports an explicit failure. The helper opens textures on Starfield's adapter; CEF choosing a different GPU may fail. The synchronous texture-copy wait is intentionally conservative. IME composition is forwarded, but candidate-window placement and accessibility are not implemented. Results on one machine do not establish broader hardware compatibility or a Starfield FPS advantage.

The backend separation was checked with both host builds, the shared DLL build, all 28 registered native tests, and the 31-suite portable native runner. Both package types passed actual DLL API acquisition, 55 compiled Papyrus signatures, and ZIP content verification. Those build/package checks are separate from the live acceptance recorded below.

Desktop checks:

```powershell
xmake test -j4 'osfui-browser-backend-tests/*' 'osfui-cef-assets-tests/*' 'osfui-cef-frame-pacer-tests/*' 'osfui-runtime-lifecycle-tests/*' 'osfui-settings-client-tests/*'
node --test tests/native/cef_bridge_tests.cjs
```

These cover local-file containment, frame deadline coalescing and stall recovery, package-based WebView2/CEF selection through the real Runtime, and both JavaScript SDKs against the injected bridge. A symlink containment check explicitly reports a skip on systems that deny symlink creation. They do not start CEF or Starfield and do not prove native callback ordering or GPU synchronization.

### Measured tradeoffs — October 17, 2026

The [desktop backend benchmark](../tools/profiling/backend-bench/README.md) completed twelve sessions on the packaged helpers from commit `ae48b6793997efae777b00d7ab24c8f960cfd275`: three repetitions per backend at 1080p and 1440p, with three seconds of warmup and ten seconds measured per workload. The machine used a Ryzen 9 7950X and RTX 3080 on Windows 11. Runtime versions were WebView2 `155.0.4283.60` and CEF `150.0.22` / Chromium `150.0.7871.256`.

1440p medians:

| Workload | WebView2 new frames/s | CEF new frames/s | WebView2 memory (MiB) | CEF memory (MiB) | WebView2 CPU (%) | CEF CPU (%) |
| --- | ---: | ---: | ---: | ---: | ---: | ---: |
| Static menu | 0.0 | 0.0 | 272 | 364 | 3.6 | 3.4 |
| Canvas animation | 52.5 | 60.1 | 290 | 385 | 13.1 | 23.6 |
| Animated menu | 47.2 | 60.1 | 329 | 407 | 28.3 | 37.3 |
| Three animated views | 54.4 | 46.5 | 557 | 677 | 47.5 | 69.6 |

Memory is summed browser-process private committed memory, not resident RAM or VRAM. CPU 100% means one logical CPU, not the whole machine. New frames count changes in the completed output's visible marker; a static menu correctly produces zero new frames. These are desktop-helper measurements, not Starfield FPS or input-to-photon latency.

Across both resolutions, WebView2 used about 60–120 MiB less private commit and 24–45% less CPU in the animated workloads. CEF stayed near 60 new frames/s with one animated view. Three-view CEF delivery varied from 46.3 to 60.1 frames/s at 1440p, so the median does not guarantee that rate on every run. CEF reached first content about 60 ms sooner in fresh-profile tests with warm OS caches. The tested RC downloads were 10.8 MiB for WebView2 and 186 MiB for CEF; the WebView2 archive does not include Evergreen.

All twelve sessions passed with valid frame markers and clean shutdowns. Both helpers also passed nine lifecycle checks, including backpressure, hide/show, message bursts, resize, and fresh-document presentation after reopen. Both exact RC packages passed stock Starfield 1.16.244 acceptance: rendered captures, keyboard/mouse input, native selects and relative drag, focus return, captured-input suppression, HUD movement, pause ownership, modern and legacy APIs, real Papyrus replies before and after load, retained state/reload, repeated opens, save/load, host termination/recovery, a 120-second soak, and clean shutdown.

The local development checkout retains the exact hashes and results under `build/backend-validation/validation.json`, the workload medians/ranges under `build/backend-validation/comparison-verified/`, and screenshots under `build/backend-validation/game/`. Earlier failed or interrupted attempts remain available separately; they are not included in the passing comparison. Older pre-refactor measurements under `build/backend-comparison/` and `build/cef-scheduling/` are historical evidence.

Remaining acceptance gaps include Luma/HDR/frame generation, physical controller and IME behavior, alternate GPU configurations, a machine without Evergreen, and the unavailable optional installed Stock Exchange fixture. The default recommendation does not imply those configurations have been tested.

CEF references: [distribution and embedding](https://chromiumembedded.github.io/cef/general_usage.html), [Windows sandbox bootstrap](https://chromiumembedded.github.io/cef/sandbox_setup.html), and the pinned SDK's `include/cef_render_handler.h` / `include/internal/cef_types_win.h` for accelerated texture ownership.
