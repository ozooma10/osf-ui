# Browser backends

OSF UI has one game DLL and one public HTML/JavaScript, C++, and Papyrus API. Two mutually exclusive packages provide WebView2 or CEF browser hosting. Both use the same view lifecycle, bridge, input policy, recovery, and D3D12 compositor. Browser engines remain in separate helper processes.

WebView2 is the recommended default and uses Microsoft's Evergreen runtime. CEF bundles its Chromium runtime and does not require WebView2. CEF is still awaiting fresh Starfield acceptance; building and packaging it does not establish rendering, input, GPU compatibility, or shutdown behavior in the game.

## Install or switch editions

Close Starfield normally and install **one** edition. With MO2, replace the previous OSF UI installation or disable it before enabling the other edition. Do not enable both editions together: they intentionally own the same plugin and configuration paths. Direct installs should remove the previous edition's browser files under `SFSE/Plugins/OSF/UI/bin` when switching, while preserving consumer views and user data.

Each archive supplies `SFSE/Plugins/OSF/UI/backend.json`:

```json
{ "schemaVersion": 1, "backend": "cef" }
```

The alternative backend value is `webview2`. Selection is read once at startup. The former `cefPrototype` player setting is ignored and no longer appears in OSF Settings. No renderer toggle or separate configuration step is needed. Missing or invalid package selection stops browser initialization with an installation diagnostic; OSF UI does not silently fall back to another engine.

CEF requires its complete matched runtime folder: the bootstrap EXE, host DLL, `libcef.dll`, supporting libraries, resources, locales, lock file, manifest, and notices. The WebView2 edition contains only its helper. Browser profiles remain separate, so browser-local storage is not migrated when switching; game/mod-owned data and the public API are unchanged.

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

Outputs are `dist/OSF-UI-v<version>-rc-webview2.zip` and `dist/OSF-UI-v<version>-rc-cef.zip`, plus SHA-256 and file-manifest sidecars. `-Tag` controls the candidate label; `-Tag ''` removes it. `-SkipBuild` uses existing binaries but still configures the requested edition, stages files, compiles Papyrus, and runs package verification. No packaging invocation deploys, tags, pushes, or publishes.

The packager installs the common payload once, then creates each edition from it. It verifies byte-identical common files, actual native API acquisition and compiled Papyrus signatures, selected-backend ownership, the CEF runtime manifest, and every file read back from each ZIP. CEF builds generate a SHA-256 runtime manifest; package verification also checks the bundled lock file against the repository pin.

## Source boundaries

- `src/Runtime`, `src/Views`, `src/Bridge`, `src/Input`, and `src/Composite` own game policy and the public API.
- `src/Render/BrowserBackendConfig` reads the package selection. `BrowserHostClient` owns shared IPC, notifications, demand, and recovery transport. `BrowserHostRuntime` handles backend-specific layout and launch preparation.
- `tools/browser_shared` contains the common protocol, messages, pipe, launch broker, local URI rules, and input data. CDP input remains private to `tools/webview2_host`.
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

Only accelerated BGRA8 rendering is implemented. A software paint callback reports an explicit failure. The helper opens textures on Starfield's adapter; CEF choosing a different GPU may fail. The synchronous texture-copy wait is intentionally conservative and needs profiling. IME composition is forwarded, but candidate-window placement and accessibility are not implemented. No claim of better reliability or performance is supported until live testing.

The backend separation was checked with both host builds, the shared DLL build, all 28 registered native tests, and the 31-suite portable native runner. Both package types passed actual DLL API acquisition, 55 compiled Papyrus signatures, and ZIP content verification. These checks do not establish a new browser or game runtime acceptance result.

Desktop checks:

```powershell
xmake test -j4 'osfui-browser-backend-tests/*' 'osfui-cef-assets-tests/*' 'osfui-cef-frame-pacer-tests/*' 'osfui-runtime-lifecycle-tests/*' 'osfui-settings-client-tests/*'
node --test tests/native/cef_bridge_tests.cjs
```

These cover local-file containment, frame deadline coalescing and stall recovery, package-based WebView2/CEF selection through the real Runtime, and both JavaScript SDKs against the injected bridge. A symlink containment check explicitly reports a skip on systems that deny symlink creation. They do not start CEF or Starfield and do not prove native callback ordering or GPU synchronization.

A separate [desktop backend benchmark](../tools/profiling/backend-bench/README.md) now exercises both actual helpers, their JavaScript bridges, accelerated shared textures, and normal exit. The October 16 comparison completed twelve sessions at 1080p/1440p using WebView2 155 and CEF 150. It favored WebView2 on median CPU, committed memory, bridge latency, and new-frame delivery; some 1440p runs varied substantially. Raw measurements and the HTML report are under `build/backend-comparison/comparison-20261016/` in the development checkout. This is desktop helper evidence; Starfield's compositor and live input acceptance remain separate.

Before the backend separation, the October 17 event-driven scheduler passed the benchmark's explicit `-Lifecycle` scenario: full-ring backpressure, responsive bridge traffic while blocked, acknowledgment-driven delivery of the latest static pixels, hide/show, a message burst exceeding one drain batch, resizing a blocked ring, destroy/reopen, and normal shutdown. Evidence and exact binary/source hashes are under `build/cef-scheduling/lifecycle-verified/`.

The pre-refactor October 17 comparison is preserved under `build/backend-comparison/event-driven-20261017-verified/`: twelve successful sessions, three repetitions per backend/resolution, with Starfield closed throughout. CEF's animated bridge RTT p95 medians are 1.14–1.54 ms, compared with 19–24 ms in the earlier polling runs; WebView2's measured medians are 0.39–0.67 ms. Single-view CEF output is about 60 distinct FPS versus 56.4 for WebView2. Three-view CEF publication is paced near 60/s, but distinct top-view delivery is only 53.0 FPS at 1080p and 51.3 at 1440p, versus 56.4 for WebView2. The 1080p CEF repetitions range from 45.9 to 54.5 FPS. Multi-view pacing remains an optimization target; capped publication does not establish 60 distinct frames per view.

CEF still uses more process-tree CPU and private committed memory in these workloads. For example, the three-view 1440p medians are 48.6% of one logical CPU and 677 MiB for CEF, versus 33.7% and 518 MiB for WebView2. These measurements describe desktop helpers, not game FPS. Earlier partial batches marked `interrupted.json` or `contention.json` are excluded. Full validation and deployment records are in `build/cef-scheduling/validation.json`.

Before broader distribution, manually verify an existing menu and HUD with CEF selected: transparency and overlapping views, show/hide/reopen, mouse drag/double-click/wheel and selects, keyboard/controller navigation, IME, Alt-Tab, resolution changes, load transitions, browser recovery, and normal game exit. Verify a legacy view and both message/state APIs, then install the WebView2 edition and confirm the same views still work. Include a machine without WebView2 and a multi-GPU machine when assessing dependency and adapter behavior.

CEF references: [distribution and embedding](https://chromiumembedded.github.io/cef/general_usage.html), [Windows sandbox bootstrap](https://chromiumembedded.github.io/cef/sandbox_setup.html), and the pinned SDK's `include/cef_render_handler.h` / `include/internal/cef_types_win.h` for accelerated texture ownership.
