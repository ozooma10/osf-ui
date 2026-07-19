# WebView2 renderer spike — Phase 1 decision report

Date: 2026-07-18  
SDK: Microsoft.Web.WebView2 1.0.4078.44 (stable)  
Runtime tested: Evergreen runtime detected by the static loader  
Phase 1 decision: **GO to Phase 2**
Final decision: **NO-GO - in-process Gate A failed inside Starfield**

## What was proven

The standalone `osfui-webview2-poc` creates a WebView2 composition
controller on an STA thread with a DispatcherQueue and Win32 message pump,
targets a `Windows.UI.Composition.ContainerVisual`, captures that visual with
`GraphicsCaptureItem.CreateFromVisual` and
`Direct3D11CaptureFramePool.CreateFreeThreaded`, and reads full BGRA frames
back through a three-slot D3D11 staging ring. CPU frames are composited with
premultiplied alpha over an animated D3D11 background.

It loads the shipped settings view through
`https://osfui.local/osfui/settings/index.html` using a virtual-host folder
mapping. The existing relative shared assets and `safeAssetSrc` paths work
under that origin; no `file://` compatibility patch was needed.

## Gate results

| Gate | Result | Measurement / evidence |
|---|---|---|
| A: pixels | GO | 1920×1080 client; 60 fps while foreground (about 120 fps when not foreground/vsync-limited); steady full-frame readback 0.70–0.75 ms average; 739,299 translucent pixels sampled with 0 premultiplication violations |
| B: keyboard | GO | Real OS click focused `Chrome_WidgetWin_0`; stand-in top-level remained active; raw input continued; typed value read back as `GateBReal`; 20/20 focus/restore cycles; Esc restored focus to `OSFUI.WebView2.POC`; F10 reacquired Chrome focus without a click and typing continued as `GateBRealX` |
| C: contract | GO | Bridge native/web/native round trip 8.00 ms; ExecuteScript succeeded; DevTools `Runtime.consoleAPICalled` observed; hover changed cursor to I-beam ID 32513; wheel/mouse forwarding did not fault; resize remained stable |
| C: lifecycle | GO | Deliberate missing virtual-host navigation failed with WebView2 error 9 (`CONNECTION_ABORTED`); destroy/recreate resumed capture (706→1000 frames); forced runtime-absent mode kept the D3D stand-in at 119.9 fps with zero captures; clean exit left zero WebView2 orphans |

![Gate A standalone capture over the animated D3D11 background](images/webview2-poc-gate-a.png)

The current x64 static-loader archive is 10,687,926 bytes, but the linked POC
executable is 395,264 bytes. The archive size is therefore not representative
of shipped size after linker dead-code elimination.

## Resource snapshot

One 1080p view produced six `msedgewebview2.exe` children. A point-in-time
snapshot of the POC plus those children was approximately 505.8 MB working set
and 419.2 MB private bytes. This is not a redistribution-size problem, but it
is materially higher runtime memory than the native shell alone and must be
remeasured in-game over the 30-minute acceptance session.

## Multi-view design sketch

Use one captured root `ContainerVisual` per renderer worker and one child
`ContainerVisual` plus composition controller per OSF UI view. View order maps
to child insertion/order under the root; hidden maps to child visibility. Only
the active controller receives `SendMouseInput` and native keyboard focus.
Each controller keeps its own WebView message/event tokens and source view ID,
while the frame pool captures the already-composited root once per frame. This
avoids N readbacks for N views and preserves the current CPU-frame compositor
contract. The Phase 2 proof should instantiate only one controller first, then
exercise this layout after the one-view in-game gate passes.
## Implementation notes and remaining risk

The browser parent must be `WS_VISIBLE` to be focus-eligible; the POC keeps
it at 1×1 so no browser HWND pixels are presented. `MoveFocus(PROGRAMMATIC)`
then works without direct key injection. The explicit
`Chrome_WidgetWin_*` `SetFocus` path remains available only as the bounded
fallback test.

The first synchronous capture implementation measured 16.77 ms because it
shared an immediate context with a vsync-blocked `Present`. A dedicated
capture device reduced this to 3.42 ms. Mapping a completed slot from a
three-texture staging ring reduced the final 1080p result to about 0.7 ms.
The ring adds two capture frames of CPU-buffer latency, which is included in
the Phase 2 interaction-latency watch item.

Still unconfirmed: Starfield's response to its child HWND holding focus,
in-game D3D12-present interaction, save/load lifecycle, 30-minute stability,
and orphan-free game shutdown. Those are Phase 2 acceptance items; Ultralight
must remain the default and untouched until they pass.


## Phase 2 implementation status

The in-process backend is implemented behind with_webview2=true and registered
as renderer "webview2". It deliberately reports itself as single-view; the
runtime loads only config.view and refuses dynamic RegisterView additions
rather than registering surfaces the backend cannot display. Ultralight
remains compiled only under its existing option and the shipped configuration
remains unchanged.

The backend starts lazily on the first native keyboard-focus request. Its worker is COM STA, owns a
DispatcherQueue and MsgWaitForMultipleObjectsEx message pump, creates a visible
1×1 child HWND of Starfield's top-level window, and owns the composition
controller, capture objects, and WebView lifecycle. WGC frames use a dedicated
multithread-protected D3D11 device and a three-texture staging ring. The mapped
BGRA frame swaps into the existing FrameBufferView handoff without an extra
full-frame copy; the D3D12 compositor is unchanged.

Implemented contract paths: virtual-host view loading, transparent background,
full-frame BGRA capture, resize/recreate of the frame pool, mouse
move/button/wheel forwarding, MessageBridge JSON in both directions,
DOM-ready/load failure delivery, ExecuteScript result callbacks,
CallJsFunction/RegisterJsFunction, DevTools console capture, cursor changes,
ProcessFailed logging, runtime-absent null fallback, and deterministic worker,
controller, capture, dispatcher, and child-HWND teardown. Native messages are
held until DOM content exists, and the document-created shim queues them until
osfui.onMessage is installed.

Keyboard uses real focus only. ApplyMenuPolicy requests
MoveFocus(PROGRAMMATIC) on open. The composition controller's
AcceleratorKeyPressed sends only framework-owned keys (rebind capture, toggle,
Esc, and dev reload) to Runtime::OnNativeAcceleratorKey; normal keys remain
unhandled for Chromium/IME. Close posts a private message to the subclassed
Starfield WndProc, which restores focus on the game's window thread. Injected
key and character methods remain no-ops for this backend.

### Build and deploy evidence

Releasedbg builds pass with the WebView2 backend enabled. The generated and
MO2-deployed DLLs were SHA-256 identical after the final implementation build:

AD0794DEAAC3B23491A73EBC97A2CDBA83AD4FA967777E81CE8F78650C1380AC

The deployed path is
C:\Modding\Starfield\MO2\mods\OSF UI\SFSE\Plugins\OSFUI.dll. During the
active Phase 2 run, both the repository and deployed MO2 configs are pinned to
renderer "webview2" so an automatic data deploy cannot silently switch the test
back to Ultralight. The source and deployed configurations are restored to the shipping Ultralight default after the NO-GO decision.

### Superseded in-game acceptance plan

This plan was blocked at Gate A before pixels. During testing, the deployed config was
already set to renderer "webview2" (the backend automatically caps the
configured two-view list to osfui/settings). Confirm the Default MO2 profile
has +OSF UI, and launch Starfield (SFSE). Verify:

1. Gameplay reaches the settings view through F10 and the unchanged Present
   hook; pixels remain transparent and correctly sized through a resize.
2. Click a text field, type, close, and reopen/continue typing without another
   click. Repeat open → type → close 20 times, checking F10 and Esc.
3. Confirm gameplay keeps rendering while Chromium owns focus and raw mouse
   input does not leak to camera movement while captured.
4. Run 30 minutes including save/load, then exit normally and confirm no
   msedgewebview2.exe process whose command line references
   %LOCALAPPDATA%\OSFUI\WebView2 remains.

Record the game version, average/peak Starfield plus WebView2 working set,
observed interaction latency, and any focus failure. Those observations decide
the Phase 2 GO/NO-GO; removing Ultralight remains separate work after GO.
## Phase 2 in-game result - NO-GO

Starfield 1.16.244 loaded the verified WebView2-enabled plugin and detected
Evergreen runtimes 150.0.4078.65 and, after an automatic runtime update,
150.0.4078.83. F10 correctly entered OSF UI capture mode, paused simulation,
showed the hardware cursor, and started the WebView2 STA worker. No UI frame
was ever submitted.

Every in-process attempt failed asynchronously in
CreateCoreWebView2CompositionController with 0x8000FFFF (E_UNEXPECTED).
The failure occurs before a controller, CoreWebView2, root visual target,
navigation, WGC capture item, or OSFUI-owned msedgewebview2.exe process
exists. It is therefore not attributable to virtual-host navigation, alpha,
CPU readback, the D3D12 compositor, or keyboard focus.

The following bounded variants produced the same result:

1. Start the STA worker during the first game tick with a visible 1x1 child of
   the game HWND.
2. Defer all initialization until the first F10 open after
   kPostPostDataLoad.
3. Create the controller against a worker-owned visible popup, then plan to
   reparent it beneath the game HWND.
4. Match the standalone POC hierarchy exactly: a visible 1x1 child under a
   visible same-STA top-level bootstrap window, with reparenting only after
   controller success.

Variant 4 used the final verified/deployed DLL hash
23AD620C137D590C5FA24909E921069FD9227217BD0684CB942C2981E694F6AB.
The standalone executable continues to prove the API and capture path outside
Starfield, while the equivalent in-process arrangement fails at controller
creation. This satisfies the stated Gate A NO-GO criterion: offscreen visual
hosting cannot be established in the target process.

Decision: stop Phase 2. Keep the experimental backend and standalone POC as
diagnostic code, disabled by default. Do not remove Ultralight. Any future
WebView2 work requires a materially different architecture (for example an
out-of-process browser host and IPC/shared-texture transport), which is outside
this spike.

> **Superseded 2026-07-19:** the in-process backend below is now the
> diagnostic escape hatch (renderer `webview2-inproc`). The shipping-candidate
> WebView2 path is the OUT-OF-PROCESS host described in
> "Phase 3 — out-of-process host" at the end of this report, which needs no
> MO2 configuration at all.

## Root cause identified - NO-GO rationale invalidated (2026-07-18)

The Windows Application event log shows msedgewebview2.exe crashing with
APPCRASH 0xc0000005 (faulting module unknown, faulting address on the heap)
at the exact timestamps of every failed in-game attempt, across both runtime
versions 150.0.4078.65 and .83. Mod Organizer 2's USVFS hooks CreateProcess
in the game and injects itself into every child process; the injected
trampoline code kills the WebView2 browser process on launch, which the
client observes as CreateCoreWebView2CompositionController completing with
0x8000FFFF. This explains why all four in-process variants failed
identically while the standalone POC (launched outside the VFS) passed:
the window arrangement was never the problem.

Fix: add msedgewebview2.exe to MO2 Settings > Workarounds > Executables
Blacklist (the default blacklist already carries Chrome.exe/Brave.exe for
the same reason) and relaunch the game. The backend now logs this hint when
controller creation fails with E_UNEXPECTED. The Gate A NO-GO above is
therefore not evidence against in-process composition hosting; the in-game
acceptance list should be re-run with the blacklist entry in place before
any architecture decision. Non-MO2 installs (vanilla, Vortex) are expected
to be unaffected.

## Phase 3 — out-of-process host (zero-touch MO2 compatibility), 2026-07-19

The in-process backend had two structural problems under Mod Organizer 2:
USVFS injects into every child process the game spawns and crashes the
WebView2 broker (fixable only by a manual MO2 executable-blacklist entry),
and the CPU full-frame readback moved ~19 MB/frame at 3440x1409. Both are
eliminated by moving the whole browser stack into a separate executable,
`osfui_webview2_host.exe`, that is never a child of the game.

### Architecture

- **Process escape.** The plugin launches the host through an out-of-tree
  broker (`tools/webview2_shared/Wv2BrokerLaunch`): first
  `IShellDispatch2::ShellExecute` through the running Explorer (the host
  becomes a child of explorer.exe), falling back to a one-shot Task Scheduler
  task; plain `CreateProcess` is used only when `usvfs_x64.dll` is not loaded
  (vanilla/Vortex). USVFS therefore never sees the host or its
  msedgewebview2.exe children. In every observed run the Explorer broker
  succeeded (`host launched via explorer (usvfs=true)`).
- **Real paths only.** Brokered launchers cannot see the MO2 VFS, so the
  plugin first mirrors the host exe from the mod folder to
  `%LOCALAPPDATA%\OSFUI\bin\<plugin version>\` (versioned against
  stale-binary races) and reuses the existing views-mirror
  (`%LOCALAPPDATA%\OSFUI\views-mirror`) for the virtual-host mapping.
- **Control IPC.** One named pipe `\.\pipe\osfui-wv2-<gamePid>-<nonce>`,
  ACL'd to the owner (`D:P(A;;GA;;;OW)`), length-prefixed UTF-8 JSON
  (`tools/webview2_shared/Wv2Protocol.h`): init/navigate/resize/hidden/
  focus/mouse/synthetic-key, MessageBridge both ways, eval + results,
  cursor, console, DOM-ready/load events, accelerator round-trip,
  frame/textures announcements, shutdown/bye.
- **Frame transport — GPU, not CPU.** The host captures the composition
  visual with WGC and copies each frame into a 3-slot ring of NT-handle
  shared BGRA textures (`D3D11_RESOURCE_MISC_SHARED |
  D3D11_RESOURCE_MISC_SHARED_NTHANDLE`), duplicating the handles directly
  into the game process. The game-side D3D12 compositor opens them with
  `ID3D12Device::OpenSharedHandle` and samples the slot directly in the
  present hook — no CPU readback, no upload. NOTE: the original plan called
  for keyed mutexes, but an ID3D12Resource opened from a keyed-mutex D3D11
  texture exposes no IDXGIKeyedMutex; the documented cross-API sync is
  shared fences, so the design uses a produce fence (host D3D11 signal →
  game queue `Wait` before sampling) and a consume fence (game queue
  `Signal` after the draw → host waits before rewriting a slot; frames the
  game skips are released by a pipe `frameAck` instead, since the game
  needs no device for that). The keyed-mutex creation path remains as a
  driver fallback only.
- **Reveal contract.** On unhide the host republishes the newest frame under
  a fresh serial (Chromium suspends rendering while hidden and WGC delivers
  nothing for an unchanged page), satisfying
  `Runtime::SubmitFrameIfVisible`'s fresh-serial reveal gate.
- **Input & focus.** Real-focus model preserved across processes: the host
  creates the visible 1x1 browser parent HWND and cross-process
  `SetParent`s it beneath Starfield's top-level window (window tree ≠
  process tree; the implicit input-queue attachment makes focus and typing
  work). Mouse rides the pipe into `SendMouseInput`; framework accelerator
  keys are decided synchronously in the host from a mirrored accelerator
  state (`accelState`, pushed by `Runtime::Tick` on change) and delivered
  back over the pipe to `Runtime::OnNativeAcceleratorKey`. Synthetic taps
  (gamepad nav, Esc delegation) post WM_KEYDOWN/UP to the Chromium widget —
  something the in-process backend could not do (its InjectKeyEvent was a
  no-op, so gamepad nav never worked on WebView2; it does now).
- **Lifecycle.** Lazy warm start on the first game tick with a configured
  view; the host takes the game PID, waits on the process handle, and
  self-terminates when the game exits (SFSE has no shutdown callback, so
  this is the only reliable orphan guard — verified through a console `qqq`
  hard exit). Single-instance mutex per game PID. If the pipe breaks the
  plugin logs once and degrades to a hidden overlay; F10/Esc still work
  game-side so input can never be stranded.

### Phase 1 — standalone gates (no game), all PASS x3 consecutive runs

`osfui-webview2-host-poc` (a D3D12 game stand-in) drives the real host:

| Gate | Result |
|---|---|
| launch | PASS — Explorer broker, host parent=explorer.exe, out-of-tree |
| textures | PASS — ring opened via OpenSharedHandle on D3D12, keyedMutex=false, 911,344 opaque pixels read back cross-process |
| typing | PASS — cross-process SetParent + click-to-focus + SendInput round-trip ("GateTyping123" read back via ExecuteScript) |
| focus | PASS — 19-20/20 hide→restore→show→refocus cycles |
| resize | PASS — ring recreated at new size, frames continue |
| shutdown | PASS — clean mutual shutdown, bye received, zero orphans |

Findings: `MoveFocus(PROGRAMMATIC)` alone was flaky cross-process when the
game window was not foreground (irrelevant in-game, where the game is
fullscreen-foreground); a real click acquires focus deterministically. A
consumer must never block a window-owning thread while the host tears down
(cross-process DestroyWindow sends messages into it) — the plugin only ever
waits on its SFSE main thread.

### Phase 2 — in-game under MO2, blacklist entry ABSENT

Starfield 1.16.244 + MO2 (msedgewebview2.exe NOT in the executable
blacklist), renderer pinned `webview2`, DLL SHA-256 verified
C557F31361F38B83719F430CACBC8F2C86C5009F95CC6E9A9910FCCA17212D59:

- Warm start end-to-end during boot: views mirrored, host launched via
  Explorer, controller + capture up, first frame published while hidden.
- First F10 open: settings view visible at 3440x1409, output-matched via the
  present-hook resize path (ring recreated 1600x900 → 3440x1409).
- Repeated F10/Esc cycles: capture, hardware cursor, sim pause, focus menu
  and control layer all engage/release; accelerator round-trip (F10 close
  from inside Chromium focus) works.
- Perf at 3440x1409: present-hook overlay cost avg 0.06 ms (p95 0.10 ms,
  n=1506/5s window), zero busy-slot waits or drops across 8400+ draws — the
  19 MB/frame CPU path is gone (the "super laggy" report against the
  in-process backend does not reproduce).
- Hard exit via console quit: host observed the pipe close and exited 0
  within ~1 s; zero osfui_webview2_host.exe / OSFUI-flavored
  msedgewebview2.exe processes remained.

Across four short live sessions (user-driven open/close bursts) the game
log recorded ZERO errors; every session end — console quit, task kill
(MO2 and vanilla), normal exit — took the host down within seconds with no
orphaned processes. Still outstanding before flipping any shipping default:
one 30-minute continuous session, sustained in-game typing, and a
save/load pass (all user-driven; the mechanisms they exercise are already
individually verified). The in-process backend stays compiled behind the
same build flag as renderer `webview2-inproc` (diagnostic only).

### Phase 3b — vanilla (no MO2) smoke test: PASS

Plugin payload temp-installed into the real `Data\SFSE\Plugins` (plus the
address library) and launched via `sfse_loader.exe` directly:
`host launched via direct (usvfs=false)` — the broker is correctly skipped
without USVFS, the virtual-host mapping used the REAL Data views path (no
mirror), controller + capture came up and the view finished loading
(`https://osfui.local/osfui/settings/index.html`). Hard-killing the game
took the host down within seconds; zero orphaned processes. The temp
install was removed afterwards. Across all sessions so far, three
hard-exit variants (console quit, task kill under MO2, task kill vanilla)
each left zero `osfui_webview2_host.exe` / OSFUI-flavored
`msedgewebview2.exe` processes.

### Build / packaging

`xmake f --with_webview2=true` builds the plugin backend AND
`osfui-webview2-host` (static CRT + static WebView2 loader, self-contained);
the data deploy ships it at `SFSE/Plugins/OSFUI/bin/osfui_webview2_host.exe`.
`--with_webview2_host=true` additionally builds the standalone POC client.
Shipped default config remains `renderer: ultralight`; Ultralight itself is
untouched.

### Multi-view (2026-07-19) — implemented per the Phase 1 sketch

The out-of-process host now implements the "Multi-view design sketch" above,
and `WebView2HostWebRenderer::SupportsMultipleViews()` returns true (the
runtime's single-view cap and the `RegisterView(...) refused` path no longer
apply to renderer `webview2`; `webview2-inproc` stays single-view).

- Host: one `View` per OSF UI view — its own 1x1 child HWND (under the
  reparented host window, so per-view widget targeting and focus keep
  working), its own composition controller + WebView2, and its own child
  `ContainerVisual` under the ONE captured root. View z = child order
  (rebuilt stable-sorted on `setOrder`; ties keep creation order), hidden =
  child visibility + `put_IsVisible` (suspends Chromium for hidden views).
  Only the ACTIVE view receives `SendMouseInput`, `MoveFocus`, synthetic key
  taps, and cursor forwarding. The root visual stays permanently visible;
  WGC still captures the already-composited stack once, so N views cost one
  capture and one shared-texture ring.
- Protocol v2 (`Wv2Protocol.h`): `navigate`'s `id` creates views; view-scoped
  game->host messages (`setHidden`/`setOrder`/`setActive`/`postWeb`/`eval`/
  `destroyView`) carry `view`, and host->game page events (`domReady`/
  `loadEvent`/`webMessage`/`console`) are tagged with their source `view`.
  Messages without `view` fall back to the active view, which keeps the
  single-view POC client working unchanged.
- Plugin: per-view records (bridge permission, hidden, order) replace the
  single-view state; notifications dispatch by view id; the frame-ack
  "hidden" gate now means "every view hidden". The environment, capture,
  ring, focus model, and accelerator path are unchanged.

In-game under MO2 (no blacklist entry, DLL 422E2C40…, host 3411E4ED…): the
configured `osfui/settings` + `osfui/keybinds` layer set loads, and OSF
Animation's plugin `RegisterView('osf.animation/browser')` is accepted — the
host reports one controller per view and a single ring
(`view '…': controller ready (N view(s) hosted)`).
