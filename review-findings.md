# OSF UI code review — 2026-09-23 (branch dev @ 3d05483)

Nine reviewers, one per subsystem. **[V]** = I re-checked this against the code myself; everything else was
reported with file:line evidence by a reviewer but not independently re-checked. "uncertain" = reviewer flagged
the trigger as unconfirmed. Paths are relative to the repo root.

---

## Tier 1 — fix first

1. **[V] Shared web kit deleted by accident (838b0f2 "Update shared stuff").** `frontend/src/shared-kit/{osfui.js,osfui.css,gamepadnav.js}`
   were deleted in the same commit that pointed `tools/xmake/runtime_payload.lua:26,60` at them. xmake `os.cp` on an empty glob
   succeeds silently, so the next deploy `os.rm(views)`s and leaves `views/shared` empty → `/shared/osfui.js` 404s. osfui.js is the
   only implementation of the `sdk/osfui.d.ts` page API (`send/request/on/state`) and the only sender of `osfui.hello`, so bridge gates
   (MessageBridge.cpp:476-486) never open. `tools/package.ps1:94-96` and `tests/package/test-package.ps1:13` fail. Example panel breaks.
   Fix: `git checkout 838b0f2^ -- frontend/src/shared-kit` (or move to a tracked data path) and make `sync_data` raise on an empty copy.

2. **[V] Shared-ring slots leak permanently (two independent sources).** Host reuses a slot only when
   `max(consumeFence, ack) >= lastSerial` (tools/webview2_host/HostGraphics.inl:286-306); reset only on ring rebuild (resize).
   - Plugin renderer drops a not-yet-taken frame without acking: `SetViewHidden` (src/Render/WebView2HostWebRenderer.cpp:1804-1807),
     `SetViewport` (:1605), pre-reveal/hidden branch (:1172-1175).
   - Once taken (`TakeLatestFrame` :1659-1663) the renderer never acks; the compositor must record it. But `CacheFrame` overwrites
     an unrecorded frame (src/Composite/D3D12Compositor.cpp:421-433) and `RecordOverlay` returns early when hidden (:754-756);
     `SetVisible(false)` (:877-882) only flips a flag.
   - Effect: each close/menu-switch race pins a slot. After ~3 → host overwrites the slot being sampled (tearing); after 4 →
     "consume lagging (all 4 slots busy)", every open ends in reveal timeout until a resolution change. Repro: open/close an
     animated view ~8× without resizing, watch host log.
   - Fix: one owner of slot lifecycle (renderer "SharedFrameMailbox" returning acks-to-send + compositor per-slot recorded serial);
     ack or CPU-signal the consume fence for any frame discarded without being recorded; clear `readySerial` on hide.

3. **[V] Host navigates before per-view setup completes.** `DrainQueuedViewWork` gates only on `webView` (tools/webview2_host/HostApp.cpp:1306-1308),
   which is set when the controller arrives (:751); event handlers + bridge shim are installed later in `FinishControllerSetup`
   after the async network-guard script (:821-836, :944). `HandlePostWeb`/`HandleNavigate` (GameMessages.inl:68,183) can trigger the
   navigate in that window → page has no `window.osfui`, no DOMContentLoaded/NavigationCompleted handlers (`domSeen` never set,
   queued posts + CDP input dead), and loads before the egress guard is confirmed. Fix: gate on `securityReady`.

4. **[V] Host reports its own blocked navigations as load failures.** `NavigationCompleted` (HostApp.cpp:1103-1118) sends
   `LoadEvent{failed}` for any non-success, including navigations the origin guard cancelled (:705) and superseded navigates;
   URL is the stale `currentUrl`. Plugin `OnViewLoad` (src/Views/RuntimeViews.cpp:90-106) schedules a crash-recovery reload
   (page state lost); repeated clicks on a plain external `<a href>` exhaust the budget → `TearDownFailedView`.
   Fix: ignore `COREWEBVIEW2_WEB_ERROR_STATUS_OPERATION_CANCELED` or only report host-initiated NavigationIds; URL from `get_Source`.

5. **[V code path; in-game effect needs a log] WndProc re-entrancy guard swallows nested messages.** `thread_local g_forwardingOriginal`
   (src/Input/OverlayInputHook.cpp:25, 182-184, 361-363) routes *every* message sent while we're inside `CallWindowProcW(g_originalProc)`
   straight to the class proc, skipping our handling and every subclasser below us. `WM_SETFOCUS` is the only focus-gain signal
   (:221-222) and on alt-tab it is normally sent nested inside WM_ACTIVATE's DefWindowProc → host `windowActive` stays false →
   mouse/keyboard/text input dropped (HostApp.cpp:1478, CdpInput.inl:61) until the menu is reopened. Guard was added for
   BetterConsole (4b1c249). Fix: drop it and rely on `OriginalMovedAboveUs`, or only short-circuit an identical re-entrant
   (msg,wParam,lParam) tuple.

6. **[V] A folder name can crash the game at boot.** `OnSFSEMessage` (src/Core/Plugin.cpp:61-81) has no try/catch (OnLoad and the tick do);
   kPostPostLoad does view discovery. `ViewManager.cpp:29` / `ViewManifest.cpp:34-35` call `path::string()` (ACP conversion, throws
   `std::system_error` on unmappable chars) *before* id validation. A `views/模组/…` folder on a cp1252 system → exception into SFSE
   dispatcher → crash. Related: `src/Core/Paths.cpp:16-17` uses ANSI `GetModuleFileNameA` (non-ACP install path → nothing found; >260 truncates);
   `Json.cpp:37` calls `.string()` inside a catch. Fix: try/catch the switch; `Utf8Path` everywhere; restrict discovered mod/view
   folder names to URL-safe ASCII (also fixes #T2-14).

---

## Tier 2 — correctness, medium

[Current item-by-item assessment and validation](#tier-2-assessment-and-changes-2026-09-23) appear below the original findings.

1. **[V] HUD lifecycle across loads is undefined.** `CloseAll` clears HUDs (src/Views/ViewPresentationController.cpp:77-82); autostart is
   one-shot at PostPostLoad (src/Runtime/Runtime.cpp:137-151). LoadingMenu/MainMenu → CloseAll (src/Input/MenuEventSink.cpp:52) → autostart HUD
   gone for the session. But MenuEventSink is only installed lazily on the first *capturing* menu (Runtime.cpp:257-275), so in
   HUD-only sessions the HUD instead stays over main menu/loading screens and chargen geometry is never detected. Renderer failure
   (:880), reveal timeout (:1006), hotkey-suppression failure (RuntimeInput.cpp:238) also CloseAll HUDs. Decide semantics:
   separate "suspend" from "close" for HUDs, re-queue eligible HUDs after transitions/rehydrate, install the sink unconditionally.
2. **HUD reveal timer covers cold host start.** HUDs `Open` immediately (Runtime.cpp:463-464) arming the 3 s reveal gate
   (ViewRevealGate.h:12) across host spawn + WebView2 env + ViewCache::Prepare; timeout → CloseAll (:1001-1006). Menus wait for load. Gate HUDs the same way.
3. **[V] `RegisterView` + `openOnStart` ignores kind and `debugOnly`** (Runtime.cpp:641-642) — opens menus (pausing the game) and debug HUDs for
   non-developers; contradicts manifest.schema.json:50/60 and MIGRATION.md:33. Use `HudAutoStartEligible` (RuntimeViews.cpp:207). ViewManifest.h:42 comment is stale.
4. **[V] Browser-host restart budget never exhausts** for a host that crashes after loading: any LoadEvent → `Reset()` (src/Render/BrowserHostRecovery.h:24-30,
   via RuntimeViews.cpp:66-69) → infinite Dead→restart→Load loop, each cycle stalling main thread in Stop(true). Reset only after a healthy window.
5. **[V] `Stop()` can wedge lifecycle at `Stopping`** (WebView2HostWebRenderer.cpp:1364-1366: exchange-then-check) → host never starts again. Check `Stopped` before exchanging.
6. **Oversize pipe write = dead pipe.** `WriteMessage` returns the same false for "too large" and "broken" (tools/webview2_shared/Wv2Pipe.cpp:414-417);
   host marks pipeDead (HostApp.cpp:328-334), plugin writer SignalDead (WebView2HostWebRenderer.cpp:512-519). A devMode `console.log` of a big object
   or a ≥8 MiB `SendToWeb` kills the host / all views. Typed result + drop-and-warn; cap producers.
7. **Any CDP hiccup kills the overlay for the session.** One failure or 5 s timeout (Wv2CdpInput.h:25,31) → `Fatal{cdp-input}` (CdpInput.inl:22-28);
   Runtime treats non-"host-connection" stages as non-retryable (Runtime.cpp:863-875). DevTools breakpoint + keypress = dead overlay. Make it per-view, non-fatal.
8. **Host init failures idle silently** — BeginEnvironment (HostApp.cpp:554-611, return ignored GameMessages.inl:51), env-completed failure, RequestController,
   put_RootVisualTarget, StartCapture, child HWND, empty userDataDir: log and return, no Fatal; host keeps heartbeating, overlay opens to nothing. One `FailHost(stage,hr)`.
9. **New host views inherit `Init.hidden`** (GameMessages.inl:18, HostGraphics.inl:422) while plugin ViewRec defaults hidden → later views skip the reveal
   handshake and `SetViewHidden(id,true)` early-returns so they can't be hidden. Always start hidden; drop `Init.hidden`.
10. **Stale deferred replies cross into a new document.** `OnViewCreated` doesn't reap `_pending` (MessageBridge.cpp:553-560); ditto BridgeApi `_inflightRequests`,
    Papyrus `viewRequests`. Page request ids restart at `q1` per document → after hot reload/recovery an old Papyrus reply resolves the new page's `q1`. Stamp a document generation.
11. **Endpoint precedence contradicts the psc contract.** Resolution tries exact native name, then `<mod>.<name>` native, then Papyrus (MessageBridge.cpp:208-231, 259-277) vs
    OSFUI.psc:71 "own namespace always wins"; no cross-registry conflict check. Plugin A's global `refresh` shadows mod B's Papyrus `refresh`. One resolver, owner-qualified first.
12. **Ready callback doesn't re-fire on recreate** despite sdk/OSFUI.h:25 ("or is recreated"): `_readyFired` only reset in SetReadyCallback (BridgeApi.cpp:223, 434-439). Plus a small race (:222 vs :435/:471).
13. **Case handling inconsistent.** SendToWeb/Register* match view ids case-sensitively (BridgeApi.cpp:189,195,427,430) vs OSFUI.h:96; `ViewManager::Find` is
    case-insensitive but all per-view maps are case-sensitive (ViewManager.cpp:69; BeginViewOpen keeps caller casing) → `RegisterView("Acme.Widgets/hud")` for folder
    `acme.widgets` never shows. Papyrus event names keep BSFixedString first-interned casing (PapyrusApi.cpp:592). Canonicalize once at the boundary.
14. **Mod ids with spaces/non-ASCII can never load**: `IsTrustedViewDocumentUri` compares canonical percent-encoded path with raw id (tools/webview2_shared/Wv2LocalUri.h:115-116;
    `IsValidModId` allows them, src/Core/Ids.h:49-74). Fails safe, but silent. Restrict ids to URL-unreserved ASCII.
15. **[V] Game-path deploy deletes other mods' views**: with `XSE_SF_GAME_PATH`, installdir = `<game>/Data` (lib/commonlibsf/xmake.lua:105-106) and
    `runtime_payload.lua:24` `os.rm(views)` wipes every mod's views. Remove only `views/shared`. (MO2 mode is safe.)
16. **Network deny has gaps** (uncertain parts): ws/wss aren't filtered at the network layer (resource filters are http/https only, HostApp.cpp:875-885; only
    network-guard.js blocks WebSocket/WebRTC); same-origin about:blank iframes may escape the script; NewWindowRequested (:1087-1093) opens any gesture-initiated
    https URL in the default browser (certain). Add `--host-resolver-rules="MAP * ~NOTFOUND"` / black-hole proxy + WebRTC policy via AdditionalBrowserArguments.
17. **Stuck keys after capture (uncertain, verify in-game)**: during capture every WM_KEYUP / WM_INPUT is swallowed (OverlayInputHook.cpp:231-253, 333-339) → release of a key
    held before capture never reaches the game (keep walking/firing); Esc/B release after close leaks to game/PauseMenu.
18. **Broker COM calls unbounded** (Wv2BrokerLaunch.cpp:51-123,130-209) on the renderer worker that `Stop()` joins unconditionally → hung Explorer under MO2 hangs the game thread.
19. **UI-pass handoff window unbounded until first candidate** (src/Composite/UiPassPolicy.h:93-106, UiPass.cpp:320-352; uncertain trigger — e.g. Luma SDR) → overlay drawn into next frame's scene targets.
20. **`g_drawEnabled` install race** (UiPass.cpp:163-165 vs 470-471): render-thread self-test failure can be overwritten by `Install()`'s final store → views open that can never draw. Derive from one state.
21. **Seam draw leaves root sig/PSO/RTV/viewport bound** (D3D12Compositor.cpp:796-812) — deliberate since 6a383c9, but the design doc was deleted (c73d5ab) so the invariant is now undocumented. Add the comment at `RecordOverlay`.
22. **Host `PublishFrame` drops a capture if the last slot isn't consumed and never retries** (HostGraphics.inl:294-295; uncertain) → end of a short transition can be lost until the next content change.

### Tier 2 assessment and changes (2026-09-23)

The numbered findings above retain their original line references. These verdicts reflect the current checkout; validation below distinguishes source/test evidence from runtime proof.

| Item | Verdict | Resolution and evidence |
| --- | --- | --- |
| 1 | Valid; fixed | Menu events now install independently of capturing menus. Loading/main-menu transitions suspend requested HUDs and close menus. HUD intent survives transitions and renderer failures; explicit closes still clear it. Recovery waits for each HUD's load before reopening. `ViewPresentationController` tests cover suspend/resume and explicit close. |
| 2 | Valid; fixed | HUD opens now wait in the existing pending-open barrier until their document has loaded. Cold host/environment startup therefore precedes the three-second presentation/reveal budget. |
| 3 | Valid; fixed | `DrainViewRegistrations` uses `HudAutoStartEligible`, matching startup discovery's HUD/debug eligibility. Corrected the manifest field comment. |
| 4 | Valid; fixed | A successful replacement load retains the restart count. The budget resets only after 60 seconds of healthy operation. Failed loads do not count as recovery; stale loads outside the response phase cannot revive exhausted recovery. Native tests cover repeated load/crash cycles and the stable interval. |
| 5 | Valid; fixed | `Stop` uses a compare/exchange transition that leaves an already stopped renderer stopped and does not duplicate an in-progress stop. |
| 6 | Valid; fixed | Pipe writes distinguish invalid payloads from disconnects. Both peers drop oversized messages without marking the connection dead. Native events and host console output have producer limits. Windows pipe tests verify a successful round trip after empty/oversized rejection. |
| 7 | Valid; fixed | CDP queue failure reports a failed load for the affected view and uses view recovery. It no longer emits a session-wide fatal error. |
| 8 | Valid; fixed | `FailHost` reports the failed stage/HRESULT and terminates initialization. Environment/options/controller, visual-target, child-window, capture, bridge-shim, and empty-user-data failures no longer leave a silently heartbeating host. Earlier graphics/bootstrap failures already exited. |
| 9 | Valid; fixed | Every host view starts hidden. Removed `Init.hidden` and bumped the private host protocol to 18; plugin and host must be deployed together. Message round-trip tests reflect the new shape. |
| 10 | Valid; fixed for runtime document recreation | `OnViewCreated` retires the previous document's deferred tokens and invokes adapter cleanup. Native inflight records and Papyrus request records are removed. Existing process-unique defer tokens make late replies harmless without adding a second generation counter. A regression test recreates the view, reuses page ID `q1`, and verifies that only the new token can answer it. |
| 11 | Valid; fixed | A shared resolver checks owner-qualified native/Papyrus handlers before global handlers, including wrong-kind checks. Cross-registry claims reject native/Papyrus name collisions in either registration order. Tests cover owner fallback precedence and conflicts. |
| 12 | Valid; fixed | Bridge replacement and runtime document recreation re-arm readiness. A revision checked under the callback mutex prevents an older pump snapshot from invoking a replaced callback. Tests cover host reconnection and recreation under the same view ID. |
| 13 | Valid; fixed | Runtime opens resolve to the discovered manifest ID before accessing per-view maps. API callback keys and outgoing event matching are case-insensitive; delivery retains canonical discovered IDs. Papyrus event names are explicitly lowercase rather than dependent on string-interning order. Added mixed-case API coverage and updated the public comments. |
| 14 | Already fixed before this pass | Tier 1's existing ID/path validation rejects spaces and non-ASCII mod IDs, with UTF-8-safe path conversion and native coverage. No additional behavioral change was needed. |
| 15 | Valid; fixed | Deployment removes only OSF UI's `views/shared` subtree. Consumer mod folders survive both direct-game and MO2 deployment. Updated the package ownership assertion. |
| 16 | Valid policy gap; hardened | Removed page-triggered external browser launches. Added a dead proxy, disabled implicit loopback bypass and QUIC, denied host resolution, and prohibited non-proxied WebRTC UDP. These process-wide controls supplement the existing resource/script guards, including for WebSockets and child frames. Live WebView2 egress behavior remains untested in this pass. |
| 17 | Partly confirmed; keyboard fix applied | Legacy and raw keyboard paths now retain ownership from press through release. A game-owned release crosses capture; an overlay-owned release/repeat remains swallowed after close. Keys held when the hook installs are seeded as game-owned. Native tests cover both transitions. The claimed mouse/fire and gamepad-B engine symptoms remain unconfirmed and require an in-game reproduction; this is not a claim that all input paths have been validated. |
| 18 | Valid; fixed | Explorer/Task Scheduler COM calls run in an owned disposable launcher process. The renderer waits at most ten seconds and polls cancellation every 50 ms, so `Stop` no longer joins a worker stuck inside those COM calls. Cancellation terminates only that helper. Hung-broker injection has not been run. |
| 19 | Valid missing bound; fixed | The barrier handoff window expires after four calls even when no candidate was seen. A native regression covers that case. The proposed Luma trigger remains unverified. |
| 20 | Valid race; fixed | Removed the separate draw-enable latch. Drawing/open eligibility derives from installation and hook state, so the final install path cannot overwrite a render-thread failure. |
| 21 | Valid documentation gap; documented | Added the narrow final-Scaleform-draw seam invariant at `RecordOverlay`, explaining which subsequent pass state is rebound and why only descriptor heaps are restored. No blanket state restoration was added. |
| 22 | Original condition stale; remaining issue valid and fixed | Earlier ring changes had already removed the specific last-slot check, but saturation could still discard the final capture. The host now retains one owned latest capture and retries on acknowledgement. Newer captures replace it; stale presentation epochs and resized dimensions are discarded. GPU/runtime behavior still needs a host/game check. |

Network controls use Chromium's documented [proxy/WebSocket and loopback-bypass behavior](https://github.com/chromium/chromium/blob/main/net/docs/proxy.md), [WebRTC command-line switch](https://github.com/chromium/chromium/blob/main/content/public/common/content_switches.cc), and [non-proxied UDP policy](https://chromium.googlesource.com/chromium/src/+/376fc41e87a058f7a7b300b0ec3a4982b4ec0960/components/policy/resources/templates/policy_definitions/Miscellaneous/WebRtcIPHandling.yaml). Their integration here is source-reviewed and compiled, not live egress-tested.

Validation completed:

- `xmake build 'OSF UI'`: plugin, host, and Papyrus compiled successfully and deployed to `C:\Modding\Starfield\MO2\mods\OSF UI`. Built/deployed DLL, host EXE, and PEX SHA-256 hashes matched.
- `tests/native/run.sh`: all 28 suites passed (43 translation units), including 58 bridge API checks. Log: `build/tier2-native-tests.log`.
- `xmake build wv2-pipe-tests` / `xmake run wv2-pipe-tests`: 101 checks, zero failures.
- `tests/package/test-package.ps1`: passed against a fresh isolated install at `build/tier2-install-check-20260923`. Running it against the existing MO2 tree first rejected the pre-existing legacy `SFSE/Plugins/OSFUI` directory; that directory was left intact.
- `git diff --check`: clean.

Browser/game smoke tests were not run; repository instructions limit this task to compilation and native verification. In-game acceptance remains outstanding for transitions/recovery, input ownership, capture retry, and the render seam. Tier 3 is outside this pass.

## Tier 3 — correctness, low (abridged)

- Listener registered before Initialize (Plugin.cpp:86-103): if load returns false, SFSE FreeLibrary's but keeps the listener → crash.
- `EnsureWebRuntime` half-built states (Runtime.cpp:191-229): `_renderer` assigned before Initialize; UiPass failure leaves objects set; exception leaves `_webRuntimeInitializing` stuck.
- kPostDataLoad/kPostPostDataLoad threading inconsistency (Runtime.cpp:241-249).
- Pending menu open survives load failure → pops open ~20 s later after recovery (RuntimeViews.cpp:90).
- Cold-open timing not cancelled on Back → bogus timing log (Runtime.cpp:543-558).
- Request ordering lost across types in one frame (ViewRequestQueue.cpp:27-37) — CloseAll applied before an earlier open.
- `Json::Get` narrows 64-bit ints without range check (src/Core/Json.h:56-66) — width/order wrap.
- Stale Load after Dead fakes recovery (WebView2HostWebRenderer.cpp:436-447); bootstrap snapshot vs PublishConnected gap loses setter diffs (:969-1005).
- Dev reload serves stale mirror for views opened after an edit (DevViewReloadWorker.cpp:75-79); `SyncTree` case-rename deletes files (DevViewFiles.cpp:132-186).
- SendToWeb validates with comment-tolerant parser but splices raw text (BridgeApi.cpp:186,195).
- Papyrus request deadline 10 s hard-coded = page default (PapyrusApi.cpp:280); capacity refusal misreported; in-flight dropped on load without settling.
- `BoundedEcho` truncates event names >128 B (MessageBridge.cpp:27-30) though qualified names reach ~193 B.
- Unregister* don't wait for in-flight dispatch (BridgeApi.cpp:94-166) → UAF if caller frees context from another thread.
- WM_APP 0x8049/0x804A on a window we don't own (OverlayInputHook.h:18-20) → use RegisterWindowMessage.
- `CursorShape` maps SystemCursorId 0 to none (CursorShape.h:25) — 0 likely means custom CSS cursor (uncertain).
- SimPause unconditional decrement of u32 pause counter (SimPause.cpp:39; uncertain).
- `FocusMenu kVtblSlots = 32` copies 5 pointers past IMenu vtable end (FocusMenu.cpp:117).
- WndProc install publishes before globals set (OverlayInputHook.cpp:380-383); F12 consumed in dev mode when overlay closed (RuntimeInput.cpp:30-33).
- Host: `Send` oversize → pipeDead; ReportSecurityFailure early-return fails open after rendererFatal (HostApp.cpp:666); callbacks found by id not generation (:625-629, 933-946);
  `ResolveView` falls back to inputTarget (HostGraphics.inl:407-413); `queuedPostWeb` unbounded; EnsureRing partial failure leaks remote handles; destroyView doesn't ReconcileCdpFocus; Ctrl+wheel zoom enabled.
- Ring adoption drains the whole GPU on the game thread under drawMutex (D3D12Compositor.cpp:194-216, 641-653); candidate filter ignores END_ONLY barriers / array targets (UiPass.cpp:96-111).
- Build/test: run.sh `exit "$failures"` wraps mod 256 (tests/native/run.sh:163); package.ps1 mutates env without restore (:50,53-54); CREDITS.md listed but deleted (:78);
  nlohmann_json unpinned in prod vs pinned in tests (xmake.lua:14); `bridge_api_tests.cpp:119-124` indexes after a failed size CHECK.

### Tier 3 assessment and changes (2026-09-23)

The original lines above are retained as the review record. Verdicts below refer to the current checkout and to source-level behavior; compilation and native tests do not establish fresh in-game behavior.

| Item | Verdict | Resolution |
| --- | --- | --- |
| Listener before `Initialize` | Valid ordering hazard; fixed | Register the SFSE listener only after runtime initialization succeeds. The proposed SFSE unload consequence was not reproduced. |
| `EnsureWebRuntime` partial state | Valid; fixed | Construct renderer, compositor, and bridge before publishing each member. A scope guard clears the initialization latch on every exit, and a separate ready bit prevents partial objects from counting as success. Failed stages can retry. |
| Data-load message threading | Valid data race; fixed | `kPostDataLoad` already uses an atomic work latch. `kPostPostDataLoad` now publishes an atomic ready flag consumed by the main tick. |
| Pending menu after load failure | Valid; fixed | A failed menu load cancels its pending open. HUD retry intent remains intact. |
| Cold-open timing after Back | Valid; fixed | Back, CloseAll, and explicit plugin close discard timing for the closed request. |
| Request order across types | Valid; fixed | One queue retains Back, CloseAll, open, and relative-pointer order from local producers. A regression covers an open followed by CloseAll followed by another open. |
| `Json::Get` integer narrowing | Valid; fixed | Signed and unsigned integer reads use `std::in_range` before conversion. Native checks cover values outside 32-bit and signed 64-bit targets. |
| Stale load after Dead | Valid race; fixed | The renderer ignores load notifications after connection death; reset already clears the old notification queue. |
| Bootstrap snapshot gap | Valid race; fixed | Snapshot publication and every state setter's enqueue now share `stateMutex`, so a setter diff cannot fall into the disconnected gap or overtake a newer snapshot. |
| Dev mirror for newly watched view | Valid; fixed | First watch refreshes the mirror and reloads after success, covering source edits made while the view was unwatched. |
| `SyncTree` case rename | Valid on Windows; fixed | Stale-path pruning compares relative paths using Windows ordinal case-insensitive comparison, so it cannot delete the just-copied destination after a case-only rename. |
| `SendToWeb` comment JSON | Valid; fixed | Store the parsed, serialized payload instead of splicing the original comment-bearing text. A native bridge check sends commented JSON. |
| Papyrus deadline | Valid mismatch; fixed | The Papyrus request ledger now uses the bridge's 30-second runtime deadline; the page's default timer remains 10 seconds. |
| Papyrus capacity result | Valid; fixed | Capacity refusal propagates a `request-capacity` reply instead of an unavailable-endpoint reply. |
| Papyrus requests on game load | Valid; fixed | Mark outstanding requests as rejected with `game-load` and drain them through the normal reply batch; late Papyrus answers cannot settle them. |
| Qualified event name truncation | Valid; fixed | Event encoding allows a 64-byte mod id plus `.` and a 128-byte endpoint name. A native check covers a 190-byte qualified name. |
| `Unregister*` and active callback | Valid; fixed | Relative-pointer, preflight, and lifecycle dispatch hold a recursive callback gate through invocation. Unregister waits for another thread's active callback before returning. |
| Private `WM_APP` values on game HWND | Valid collision risk; fixed | Both processes use named `RegisterWindowMessageW` IDs for focus restore and input refresh. |
| `SystemCursorId == 0` | Valid; fixed fallback | [WebView2 uses zero for custom CSS cursors](https://learn.microsoft.com/en-us/microsoft-edge/webview2/reference/win32/icorewebview2compositioncontroller?view=webview2-1.0.3912.50). Map it to a visible arrow because the IPC does not carry custom `HCURSOR` data. Exact custom cursor shapes remain unsupported. |
| SimPause unsigned decrement | Plausible on a reset counter; guarded | If the engine counter is already zero at release, clear our latch without decrementing. A live load/reset interleaving was not reproduced. |
| FocusMenu vtable copy | Valid; fixed | Copy 27 slots, the `IMenu` declarations through `0x1A`, instead of reading five entries beyond that table. |
| WndProc install publication | Valid; mitigated | Seed the class proc and predecessor before swapping WndProc, and use atomic pointers while the window thread forwards. A simultaneous third-party subclass between `GetWindowLongPtrW` and `SetWindowLongPtrW` still needs live compatibility evidence. |
| F12 while overlay closed | Valid; fixed | Developer F12 is consumed only while overlay input is captured. |
| Host oversize `Send` | Already fixed in Tier 2 | `Pipe::WriteResult::InvalidPayload` drops the message without setting `pipeDead`; no further change. |
| `ReportSecurityFailure` early return | Invalid as stated | The first failure sets `rendererFatal` and `quit`, sends Fatal, and stops the WebView. Later callbacks return into an already terminating host. |
| Async callbacks found only by id | Valid; fixed | Controller and network-guard completions also check the instance generation; an old completion cannot initialize a recreated view of the same id. |
| `ResolveView` input-target fallback | Valid; fixed | Messages lacking a known explicit view now resolve to no view, so stale control messages cannot affect the input target. |
| `queuedPostWeb` growth | Valid; fixed | Bound each view's pending posts to 64 messages and 8 MiB, dropping oldest with a single warning. |
| `EnsureRing` partial failure | Valid; fixed | Failed duplication or announcement closes handles already duplicated into the game process before releasing the local ring. |
| Destroy view and CDP focus | Valid; fixed | Reconcile CDP focus after replacing a destroyed input target. |
| Ctrl+wheel zoom | Valid; fixed | Disable WebView2 zoom controls for hosted views. |
| Ring adoption drains GPU | Stale in current code | `TakeRingIfIdle` defers adoption while reads exist; `EnsureSharedRing` no longer calls a GPU-idle wait. The draw mutex still protects resource replacement. |
| UI-pass candidate filter | Valid guard gap; fixed | Ignore `END_ONLY` transitions, which [complete a split barrier while the resource has been pending](https://learn.microsoft.com/en-us/windows/win32/direct3d12/using-resource-barriers-to-synchronize-resource-states-in-direct3d-12), and texture arrays; require an all-subresource or subresource-zero transition. A concrete game trigger remains unverified. |
| Native runner exit count | Valid; fixed | Report the count but exit 1 for any failure, avoiding shell exit-code wrap at 256. |
| Package environment | Valid; fixed | Restore all three environment variables in `finally`, including on packaging failure. |
| Deleted `CREDITS.md` | Stale optional entry; cleaned | Removed the nonexistent file from the optional document copy list. |
| JSON package version | Valid reproducibility gap; fixed | Production XMake now requests the same nlohmann/json 3.11.3 version as native tests. |
| Bridge test indexing | Valid test crash risk; fixed | Index the sent-message vector only after its size check succeeds. |

Validation: `xmake build 'OSF UI'` compiled the plugin and host and deployed to the configured MO2 mod; built/deployed DLL, host EXE, and PEX SHA-256 hashes matched. All 29 native suites passed; `tools/package.ps1` parsed successfully; `git diff --check` was clean. Browser and game smoke tests were not run. The items noted as needing live evidence remain open for runtime acceptance.

---

## Maintainability — structural themes (highest leverage first)

1. **Frame/slot ownership** — extract `SharedFrameMailbox` from the renderer (all `haveFrame` transitions, returns acks) + per-slot recorded/signalled serials in the
   compositor. Fixes Tier-1 #2 by construction and lets you delete the idle fence, `Retire`, and the `HasPending`/`WaitForGpuIdle` double guard.
2. **Runtime god object** (~80 methods, ~55 fields, 2.1k lines across 5 dirs: Runtime.cpp, RuntimeFrame.cpp, Input/RuntimeInput.cpp, Views/RuntimeViews.cpp, Bridge/RuntimeBridge.cpp).
   Extract `ViewOpenCoordinator` (pending open, preflight barriers, cold-open timing — removes 4 duplicated close-all sequences + 7 scattered timing cancels),
   `RelativePointerSession`, browser-host lifecycle, and a narrow input-sink header so OverlayInputHook/MenuEventSink don't pull 19 headers. Document which thread owns each field.
3. **View state in parallel maps** (PresentationController `_instantiated`, ViewLoadTracker, ViewRecoveryTracker, Runtime `_pendingViewOpen`/barriers/fault counts, dev worker)
   with inconsistent case rules → one `ViewRegistry` of `ViewRecord`s under a canonical id. Collapse the three drifted "navigate view" sequences
   (RuntimeViews.cpp:35-57, 109-122; Runtime.cpp:837-840) into one `NavigateView()`; the trailing Resize/SetViewport calls are no-ops.
4. **Endpoint routing** is split across platform / native plugin / Papyrus registries with duplicated resolvers (6 copy-pasted wrong-kind blocks, `kMaxWarnedEndpoints` ×2),
   a hand-maintained reserved-name list, and two JSON→Papyrus arg parsers (RuntimeBridge.cpp:18-90 vs PapyrusCall.h:46-107, different limits/types). One resolver + one parser + one limits header.
5. **HostApp.cpp** splices three `.inl` files into one ~90-member `struct App` with implicit per-view flags (`controllerRequested`, `securityReady`, `domSeen`, `revealPending`, `hideDeferred`).
   Real classes: `BrowserView` with an explicit setup state machine (would have prevented Tier-1 #3), `CaptureRing` (only cross-thread state), `BrowserPolicy`, `InputRouter`, `HostMain`.
6. **WebView2HostWebRenderer.cpp (1844 lines)** → `BrowserHostDeployment` (cache/mirror/lease/exe mirror/MotW — filesystem-only, testable), `BrowserHostTransport`, `SharedFrameMailbox`, thin facade.
7. **Input policy** spread over five static-only classes (FocusMenu, ControlLayer, SimPause, FreeCursor, UiLayoutGuard) with the teardown tail duplicated 3× (Runtime.cpp:883-886, 1009-1012;
   RuntimeFrame.cpp:47-51) and gamepad capture hidden in `ReconcileControlLayer` → one `EngineInputPolicy::Apply(capture, pause)`.
8. **Protocol truth scattered** — make Wv2Messages.h the single source: message types via `kType` (the string-literal coalesce keys `relativePointerCapture`/`accelState` in
   Wv2BoundedQueue.h:21-22 are already stale), direction labels, size limits, a `Log.level` enum; drop unread wire fields (`Hello.hostVersion` — or log it, `Textures.keyedMutex`, `Init.hidden`, `Ready`).
9. **Defensive layering from a lifecycle that never happens** — Runtime is intentionally leaked, so `~Impl`s, graceful `Stop(false)`, compositor teardown, `Initialize()`-always-true,
   ~40 `_renderer/_bridge/_compositor` null checks, four atomics for one UiPass install state, and triple function-pointer indirection in the compositor are all dead weight.
   Construct renderer/compositor/bridge once at kPostPostLoad (host spawn + GPU setup stay lazy). Use `ComPtr` instead of ~20 `SafeRelease` sites.

## Dead code / leftovers (safe deletions, verify each with a grep)

- `InstallOverlayDrawPath` / `_drawPathRequested` (Runtime.cpp:231-239) — never installs anything.
- Ring-truncation health channel (HealthEvent/SetHealthHandler/ReportHealth, ringSlotsAnnounced/Reported; WebView2HostWebRenderer.cpp:340-348,1125-1134,1638-1648,1705-1708; Runtime.cpp:88-91; OSFSettingsClient.cpp:118-120) — can't fire.
- `session.pid` / `session.topLevel` / `SetTopLevel` / `sessionMutex` (renderer :327-334, 415-434, 849-850) — from the removed focus handoff.
- `outboundOverflowed`, `deadLogged`; `RestartAfterFailure` always true → `OnAttemptSetupFailed` dead.
- `MenuEventSink::ConsoleOpen` / `s_consoleOpen`; `Receiver_OnCharacter` probe + per-frame `Receiver_OnButton` logging (FocusMenu.cpp:52-70).
- Host: `App::options`, `gameMessageOverflow`, `shutdownRequested` (write-only), unreachable `HandleShutdown`, `physicalWheel`==`wheel`, bridge-shim `ui.visibility` branch.
- BridgeApi `TakeViewPresentationRequests/TakeViewRegistrations/TakeViewStateOps` (test-only), `PendingReply::view/name`, `InflightRequest::token/name`, unreachable per-view in-flight cap.
- `src/main.cpp:16` trampoline reservation (nothing uses it since MainThreadMenuPump removal).
- `ScavengeLegacyViewMirrors` (renderer :166-192) — full process snapshot every start for a pre-08e101d migration.
- `package.json`, `package-lock.json`, `node_modules/` (incl. broken symlink), `tools/xmake/frontend_views.lua`; `.gitignore` stale entries.
- `DeferredMainThreadWork.h` (one atomic for one field), `PendingPresentationWork`, `PublishPlatformState(key)` (only "views"), `InitializePaths` wrapper.

## Duplication

- Close-all-ring-handles loop ×4 (renderer :1112-1114, 1281-1288, 1447-1455; D3D12Compositor.cpp:320-338) → `SharedRingDesc::CloseHandles()` / owning type.
- `FindTopLevelWindow` ×2 (OverlayInputHook.cpp:57-84, renderer :231-249) — two layers could pick different HWNDs.
- `IsProcessElevated` inline copy (HostApp.cpp:1721-1732); host log-level mapping ×2; FNV mix ×2 (DevViewFiles.cpp vs ViewCache.cpp); `DevViewFiles::ModFolder` = `Ids::ModOf`;
  `kVkEscape` ×2; `kRestoreGameFocusMessage` ×2 + static_assert bridge; 32:32 size packing ×4; frame-pool create/recreate with magic `3` ×3.
- Three identical `{fn,user}` exact-view registries in BridgeApi (:85-180) → `ExactViewRegistry<Fn>`.
- Tests: 5 identical `OSFUI::Log` shims, 8 private Check/failures copies alongside `stubs/check.h`, 3 temp-fs helper copies.

## Stale comments / naming / slop

- Mixed `m_` and `_` member prefixes (Runtime.h, Views classes).
- "first main-thread tick" install comments (FocusMenu.h:16, MenuEventSink.h:14, OverlayInputHook.h:21, UiLayoutGuard.h:10) — install is lazy on first capturing menu.
- Runtime.h: :3 (unordered_map *is* in pch), :35, :47-48, :51 "pused", :97, :116 misplaced; Runtime.cpp:684 and MessageBridge.cpp:161 sentences cut off with ';'.
- ViewManifest.cpp:19 ("Nested paths identify v2" — code requires 1), :50 (claims a check that doesn't exist); DevViewFiles.cpp:127 sort comment wrong.
- `FrameBufferView` named for the old CPU pixel buffer; `frameIndex` is really the host serial (renamed at each hop).
- `OnDataLoaded` handles kPostDataLoad, `OnPostDataLoaded` handles kPostPostDataLoad.
- Compositor: `m_trackingHeaps`/`TrackingHeaps()` no longer gate anything; "(no IDXGISwapChain::Present hook)" log; "fixed tracker exhausted" comment; 200+ char one-line comments; shouting FIRST DRAW log.
- RuntimeBridge.cpp is all `Runtime::` methods living in src/Bridge, with mixed 4-space/tab indentation.
- Magic numbers without names: host timeouts (20000/30000/250/3000/1000), handoff window 2/4, 256 px min target, 2000 ms wait, vtable slots 10/26/28/7, capacity limits (64/32/256/1024/128) in ~10 places.

## Tests / build / tooling

- No single test entry point: 1 of 27 run.sh suites wired to `xmake test`; `wv2-pipe-tests`/`wv2-cdp-smoke` in neither; README lost its test command (3d05483); CI removed (c3017af).
- `runtime_lifecycle_contract_tests.cpp` greps source text; order checks pass vacuously when an anchor is renamed; will break on any Runtime refactor. Replace with behaviour tests.
- `stubs/SettingsServices.h` hard-codes export/module names instead of SDK constants; `moduleLookup` accepts any module name.
- `test-package.ps1` duplicates package.ps1's file list (already drifted); regex-greps Lua for the `os.rm(views)` line.
- Version in 3 places (xmake.lua:4, Version.h:8 + :10-12, package.json:3).
- Deploy runs twice per DLL rebuild (after_build + commonlib auto-install); `build_papyrus` runs twice.
- Coverage gaps in risky pure logic: `Ids::IsValidModId`/`IsValidQualifiedViewId`, manifest entry-path escape rule, `RetainedStateStore` caps, `ViewRecoveryTracker` backoff, `RegisterLaunchers` debug-only filter.
