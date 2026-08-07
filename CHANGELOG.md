# Changelog

## Unreleased

### Changed

- OSF UI now uses one documented vocabulary across the OSF UI runtime, SDK, authoring guides, scaffolder, and built-in views: a stable view is distinct from each browser document instance; the OSF UI runtime, browser host, web renderer, compositor, and mod backends have unambiguous roles; pinned residency is distinct from prewarming; and game input actions, bindings, mod hotkeys, engine input contexts, and hotkey contexts are named separately. Current-facing docs no longer teach the retired 1.x bridge envelope, a nonexistent manifest `id`, or a menu stack. Existing bridge fields, native ABI methods, manifest keys, CLI flags, view ids, and state keys keep their compatibility spellings. The built-in views are now consistently called **Mod Settings** and **Keybindings**, including the default **MOD SETTINGS** pause-menu entry, and Mod Settings labels authored views as **Menus** and **HUDs** instead of mixing systems, terminals, and overlays.
- OSF UI 2.0.x temporarily runs views that declare a 1.x `targetVersion`, DLLs built against the final native ABI 1.8 surface (including older 1.x prefixes), and scripts calling the six retired Papyrus natives. Each concrete legacy view, DLL, or Papyrus mod gets one persistent System Health warning and one WARN log entry naming OSF UI **2.1.0** as the removal release, so authors can migrate without their current UI disappearing. The compatibility implementation is isolated behind an explicit legacy navigation flag and adapter modules; API 2.0 views and DLLs retain the strict 2.0 behavior.
- The **Debug mode** toggle is gone from Mod Settings; third-party `debugOnly` views now follow the `devMode` flag in `config.json`, the same switch that already governs verbose logging, hot reload and F12 DevTools. While it's on, Mod Settings shows a standing amber **DEV MODE** tag next to the OSF UI release version badge, and System Health reports `devMode` in its system block (previously `debugMode`).
- **Key bindings are physically anchored and layout-aware.** A key name (`"F8"`, `"Semicolon"`) now identifies a physical key position — the same convention Starfield's own `ControlMap` uses — instead of a Windows virtual key whose position shifts with the OS keyboard layout. On US layouts nothing changes; on German, French and other layouts, rebinding, hotkey dispatch, the Keybindings view, and game-binding rows now all agree on which key is which. Keys that were previously uncapturable (the ISO `<>` key, the numpad, PrintScreen, sided modifiers pressed directly) are bindable, and configs mean the same physical keys on every machine. The binding UI shows the keycaps your layout actually prints (`Ö`, `ß`, `^`), searching by keycap works, and translation mods can localize non-printing key names via `chrome.keys.*` catalog addresses.
- Saved key values are migrated **once**, when this OSF UI release first loads them, using the keyboard layout active at that moment (a no-op on US layouts). If you bound keys under a different layout than the one active at first launch, re-check those bindings once. **Do not downgrade** to a pre-2.0 build afterwards: a rebind made there is written into an already-migrated file and will not re-migrate.
- Additive API surface: the `osfui/settings` state doc gains `keyboard: { layout, labels }` (localized keycaps per key name, republished on layout switch), `settings.captured` gains `label` (the keycap for the captured name) and `reason` on cancels (`"escape"` | `"reserved"` | `"unnameable"`), and W3C `KeyboardEvent.code` spellings (`BracketLeft`, `Backquote`, `ShiftLeft`, …) are accepted as key-name aliases. Names remain the only stored identity — labels are display-only.
- The Keybindings view now copies Starfield's live `ControlMap`, so it shows the complete localized action catalog before the game's Controls panel is opened—including unbound actions, main/alternate slots, chords, exact engine input contexts, and in-game remaps. The old curated `vanillakeys.json`, control-map text overlays, and `vanillakeys.user.json` override path are retired; existing user files are left untouched but ignored. A game-version or layout mismatch fails closed, reports an actionable System Health issue, and makes no stale game-binding conflict claims. The OSF UI runtime and browser host must update together (browser-host IPC protocol 6).
- Key settings can declare semantic `gameplayModes` (`onFoot`, `ship`, `vehicle`, `zeroG`) on their named hotkey context. Dispatch now follows Starfield's live engine input context stack; scoped bindings fail closed when that mode is unavailable, while existing schemas retain their legacy non-menu behavior. Conflict warnings distinguish hard core collisions from possible special-context overlap, allow proven-disjoint mod modes to share a key, keep menu/unknown engine input contexts display-only, and treat `blocksGameplay` reuse as shared. The **Warn about game-binding collisions** toggle hides only game-binding warnings, not the read-only catalog or mod-to-mod warnings.
- Win keys are nameable in configs but reserved from capture (a Win keyup opens the Start menu); Esc remains the rebind-cancel key.

### Security

- `osfui.papyrus.call` can no longer target OSF UI's own `OSFUI` script. Its Papyrus natives take the target mod id as an argument and trust their caller, so naming them from a view was a way to write another mod's settings, reset them, publish state under another mod's identity, or rebind OSF UI's own overlay key — all of which the equivalent `settings.*` endpoints refuse. The security model now also states plainly that the vanilla script library, including `Debug.ExecuteConsole`, is within reach of a bridge-enabled view.

### Fixed

- The complete discovered-view inventory is reachable again from OSF UI's own Mod Settings detail. It no longer depends on the retired Diagnostics settings group, so authors can inspect catalog-hidden and uninstantiated views against the current Interface settings schema.
- Settings sliders now update their readout while dragging but commit only when the drag ends, and text and colour fields commit on their native change boundary instead of on every keystroke. Preact's compatibility transform had collapsed the deliberately separate `input` and `change` handlers onto the same event, causing slider write spam and inconsistent text-field commits.
- The injected pause-menu entry no longer waits three extra menu-pump ticks after Starfield's list becomes live. Injection still runs only from the main-thread post-Scaleform pump and retains every movie-liveness, list-presence, and count guard that prevents stale AS3 access.
- Post-data-load GameVM and UI setup now runs on OSF UI's serialized main-thread checkpoint instead of directly inside SFSE lifecycle callbacks, which can arrive on a job thread. This covers Papyrus binding, UI layout validation, menu-event registration, the post-Scaleform pump hook, focus-menu registration, and the game-window input hook, removing the same false-main-thread assumption behind earlier startup and Scaleform instability.
- Starfield no longer intermittently crashes with `bad allocation` while the live keybinding catalog starts. SFSE can deliver `kPostDataLoad` from a job thread while OSF UI's main-thread tick is already running; catalog initialization is now handed off wholesale to that serialized tick instead of exposing partially built strings, vectors, and JSON across both threads.
- Searching or filtering the Keybindings list now prioritizes the same occupied keys on the keyboard map and recedes unrelated keys, so the map clearly shows which physical bindings are represented by the current list. When a key is selected, bindings from the selected Layer are listed first without hiding bindings from its other contexts.
- Enabling OSF UI no longer stalls or heavily degrades the game while the live keybinding catalog is active. The first implementation retained the reverse-engineering probe's intentionally conservative memory query on thousands of individual fields and string characters, then repeated part of that guarded path every frame; production now takes guarded bulk snapshots, caches localization across remaps, validates the tiny active-context allocation only when its pointer changes, coalesces duplicate remap notifications, and skips translation, JSON construction, conflict recomputation, and broadcasts when a dirty notification did not actually change the map.
- First-party dropdowns now stay inside their OSF UI view and remain selectable. The Keybindings filter and long Mod Settings enum controls no longer open WebView2's separate native picker window, which could extend beyond the game window and lose clicks when Chromium focus crossed OSF UI's mouse-capture boundary. Menus widen for long values and shift back inside the viewport instead of clipping labels to the trigger width.
- Two views making requests at the same time could receive each other's replies. Documents number their own requests from a per-document counter, so every document's first request is `q1`; the OSF UI runtime keyed deferred requests by that id alone, so a second view's request displaced the first, one document's promise settled with another's data, and the displaced request hung until its timeout. Deferrals are now tracked by an OSF UI runtime-minted token and the document's id is only echoed back on the wire.
- A request handler that rejects a request, or misses its deadline, no longer counts against the calling view's protocol-fault budget — an ordinary application error could accumulate into a System Health warning naming a view that had done nothing wrong.
- Menu opens are refused when the overlay cannot actually draw, not just when its install-time hooks failed. The seam's command-list hooks are taken lazily on the first frame and can fail then, which previously still admitted an invisible overlay that captured input.
- Mod Settings no longer keeps displaying a value the store refused: a rejected write is rolled back locally. A refusal never touches the store, so nothing was coming to correct the display.
- Rebinding a key repeatedly no longer leaks one `settings.captured` subscription per attempt.
- Retained mod state is capped by mod as well as by key, so the per-key cap actually bounds the store.
- A plugin refused for an ABI-major mismatch is listed once in System Health, however many times it retries.
- `create-osfui` menu and HUD projects are runnable starters rather than bridge skeletons: retained state, one-shot events, sends, typed requests and errors, live settings, hotkeys, localization, theming and lifecycle all work in the browser harness and in game, with view-kind-appropriate manifests, schemas, mod-backend callbacks, translation catalogs and mocks. They target the 2.0 authoring API throughout, pass `npm run check`, retain HUD data across document recreation, and no longer wait on dead subscriptions or mock requests that never settle.
- OSF UI can now run alongside Luma: it waits for peer SFSE render hooks before installing its own, safely chains Luma's Scaleform composite hook, and renders into Luma's upgraded HDR UI buffer. If the UI draw path is still unavailable, menu opens are refused immediately instead of briefly capturing input for an overlay that cannot appear.

### Other changes

- Removed the unfinished in-world browser-surface experiment and its dormant configuration/build switches. It required assets that never shipped, was disabled in every release, and had accumulated OSF UI runtime and packaging branches around an unsupported feature.
- Production rendering now uses the WebView2 web renderer and D3D12 compositor exclusively. Removed diagnostic null-renderer/null-compositor configuration and build switches that could produce an apparently successful plugin with no way to render.
- Removed the developer-only Web Performance Lab and the live render-stat overlay. System Health still reports whether the UI seam is active and whether Frame Generation is detected, without maintaining per-frame counters, sampling IPC, or injected page UI.
- Removed diagnostic uploads and the post-crash reporting prompt. System Health remains local and still supports copying its diagnostic report and issue details, opening logs, retrying views, and following update or troubleshooting links.

### For view authors

- `osfui check`, `build`, and `package` now validate generated manifests and drop-in settings JSON against the packaged 2.0 schemas, including malformed JSON and a settings id that disagrees with its filename. During 2.0.x the toolchain can also preview, build, and package a pre-2.0 project through the temporary 1.x façade, while printing the 2.1.0 removal warning. Malformed targets are still refused, and new scaffolds and published typings remain 2.0-only.
- The 2.0 shared kit drops exports that no shipped view or scaffold used: `--osf-bg-canvas`, `--osf-inset-lit`, `--osf-shadow-sm`, `--osf-space-1`, `--osf-space-5`, `--osf-space-9`, `--osf-steel-100`, `--osf-surface-input`, `--osf-surface-panel`, `--osf-text-2xl`, `--osf-text-4xl`, `--osf-text-lg`, `--osf-text-xl`, `--osf-tracking-mega`, `--osf-tracking-wide`, `--osf-void-400`, `.osf-section-head`, `.osf-tag`, and `.osf-tricolor`. The direct `.osf-note--info` and `.osf-note--danger` classes are also gone; schema note tones keep their existing appearance through the settings renderer.
- `@osfui/cli` and `create-osfui` now follow the current API from one source for the OSF UI release version. Built manifests no longer write the removed `id` field (the folder owns identity), the harness envelope editor accepts 2.0 `kind` messages instead of refusing them for lacking a 1.x `type`, and its built-in mock recognizes the complete current platform endpoint set with protocol-shaped replies plus the current own-mod, platform-private and OSF UI Papyrus-script gates. Programmable mocks expose `onEndpoint`; the older `onCommand` and `Command*` types remain as deprecated compatibility aliases. Generated menu examples also render key settings through the player's current keyboard-layout labels instead of presenting a raw physical key name.
- `osfui build` and `osfui package` work for scaffolded Papyrus projects again. The compiler was handed a `Scripts/Source/User` import folder that generated projects no longer create, and it hard-fails on a missing import folder — so no `.pex` was produced and the build stopped before the view step. Projects that still keep sources under `Scripts/Source/User` are unaffected.
- The generated browser mock now behaves like the generated Papyrus script: the click counter counts the same way in the harness and in game, the "Mod backend enabled" setting is honoured in both, `Greet` updates the greeting in the harness, and `Refresh` is implemented on both sides.
- The developer harness delivers events raised before a view's document greets the bridge, matching the OSF UI runtime. The mock discarded them, which quietly hid the message-before-first-paint guarantee from every harness test.
- The harness handshake reports the view's real qualified id rather than an empty string.
- `osfui.state` keys are always `"<mod>/<key>"`. The SDK types claimed a bare key would resolve against your own mod; nothing implemented that, so such a subscription silently never fired.
- Views can now call any GLOBAL function on any Papyrus script with `osfui.papyrus.call(script, function, ...args)`. Scalar arguments retain their string, integer, float, or boolean types, with `osfui.papyrus.float(value)` available when a whole-valued number must remain a float. Generated Papyrus projects therefore ship only a loose PEX and no longer need an ESM, startup quest, load-game registration, or Spriggit.
- New `npm create osfui@latest -- --surface settings` scaffolds a settings-only mod: a settings schema, a rebindable hotkey wired straight to a GLOBAL Papyrus function through the schema's `onPress` target, a translation catalog, and a script that compiles and deploys it. There is no view, no npm project, and no registration to re-arm after a save load — the whole mod is two JSON files and one `.psc`.
- Generated menu and HUD projects are deliberately smaller. Each keeps one worked example of every way a view and its mod backend talk to each other and a settings schema you would plausibly ship, and points at the authoring guides for the rest, instead of scaffolding a four-card tour and an every-widget schema to delete. The generated `FEATURES.md` is gone; each README now links the documentation that replaced it.

### Breaking changes

- **The 2.0 shared bridge helper API drops the 1.x aliases for 2.0 views.** `emit`, `call`, `action`, `viewReady`, `data.push`, `data.state` and the top-level `t`/`localize`/`locale`/`applyAccent` are absent from a view targeting 2.0. During 2.0.x only, a manifest explicitly targeting pre-2.0 selects an isolated façade that restores the final documented 1.x API and raises a 2.1.0 removal warning.
- **The native ABI is now 2.0.** `CommandFn`, `RegisterCommand`, and `UnregisterCommand` are replaced by strict `SendFn`, `RegisterSend`, and `UnregisterSend`; `RegisterRequest` remains the reply-bearing path. ABI 2 callers keep verbatim send payloads and reject request-to-send without injection or auto-ack. During 2.0.x only, ABI 1.x callers receive a frozen 1.8 adapter plus a 2.1.0 removal warning.
- **`SettingsData.vanillaKeys` is removed.** Built-in views consume the complete live game-binding catalog from the retained `osfui/keybindings` state key. The native conflict engine keeps its internal game-binding catalog.
- **Papyrus deprecates `PushToView`, `PushFormsToView`, and the four `RegisterForViewActions*` registrations.** They remain bound through 2.0.x and raise a 2.1.0 removal warning. Publish with `SetView*` (replayed to every fresh document), announce with `SendViewEvent` (never replayed), and use `ListenForViewActions{,Static}` before 2.1.0 removes the adapter.
- **Reads that secretly subscribed are gone.** `settings.get`, `views.get`, `i18n.get`, and `diagnostics.get` were requests with an invisible side effect — asking once enrolled you in every future push. They are now plain state keys (`osfui/settings`, `osfui/views`, `osfui/i18n`, `osfui/diagnostics`): subscribing replays the current value immediately and again on every change and every reload, so there is nothing to request and nothing to re-request.
- **`osfui.request()` resolves the reply payload, not the whole envelope.** This is the one break that fails quietly instead of throwing — code reading `result.payload.x` now reads `undefined` rather than crashing. Check every `request()` call site by hand.
- **Deleted from the strict 2.0 path.** `hud.show`/`hud.hide` (aliases of `menu.open`/`menu.close`), `osfui.textFocus`, `ui.action`, and `ui.papyrusRequest` are absent from a 2.0 view, as are the `runtime.ready`, `ui.result`, `ui.error`, `settings.ack`, `settings.data`, `views.data`, `i18n.data`, `diagnostics.data`, `data.push`, `data.state`, `papyrus.result`, `runtime.pong`, `game.data`, and `handoff.state` message types. Routing metadata now travels beside the payload rather than inside it, so a payload field can no longer overwrite the command name. The temporary façade reconstructs the public 1.x aliases and shapes only for an explicitly pre-2.0 view through 2.0.x.
- **`settings.set` rejects on failure** instead of resolving an `{ ok: false }` document the caller had to remember to inspect, and it resolves with the post-clamp committed value so clamped and accepted are distinguishable without a re-read. `settings.captureKey` now settles in machine time (`{ armed: true }`, or a `capture-busy` / `forbidden` / `not-rebindable` rejection) and the captured key arrives afterwards as the `settings.captured` event, however long the player takes — a request that waits on a human is the wrong shape and fights the client timeout.
- **Breaking (configVersion 2):** `config.json` no longer takes `views` or `warmViews`; there is no config-v1 compatibility branch, so removed fields are ordinary unknown keys and are ignored with the normal warning. Every valid installed view is discovered automatically (in deterministic id order) and instantiated on first use; the handoff and Mod Settings views are pinned and prewarmed internally. `view` remains the default menu for the toggle key. Saved setting-key migrations and unknown setting-value preservation remain intact.
- **The five input boot switches are removed.** `inputSource`, `captureInput`, `hardwareCursor`, `focusMenu`, and `engineInput` are now ordinary unknown `config.json` keys. OSF UI always uses its WndProc input hook, hardware cursor, FocusMenu capture policy, and engine-routed gamepad path; a menu that captures input is refused if that complete integration is unavailable, while non-capturing HUDs remain usable.

### Fixed

- Repeatedly closing and reopening a menu can no longer let a transparent frame from the previous closed state satisfy the next reveal. OSF UI waits for a frame from that exact opening and closes the menu after three seconds of live game time if one never arrives, preventing an invisible overlay from trapping input and pause state. Time spent alt-tabbed or in a load hitch does not count against that deadline, and a reopen that changes nothing on screen re-sends the current pixels so a static page still reveals instantly.
- Losing or stranding the browser host now closes the overlay and releases input and pause, then starts a fresh browser host with bounded retries and recreates every previously instantiated view without restarting Starfield. Recovery leaves the overlay closed for the player to reopen; after the automatic budget is spent, the next menu-open request starts a fresh retry cycle. The browser host also exits when the game window has disappeared even if its process or pipe watcher misses the exit — re-attaching first if the game merely recreated its window.
- Shared browser textures are no longer reused before Starfield's GPU has actually submitted the draw that reads them. A stalled consumer now drops a browser frame instead of overwriting live pixels, and ring changes remain deferred when GPU idleness cannot be proven.
- Quitting Starfield no longer runs OSF UI's blocking worker teardown or releases game-owned VM, Scaleform, and interned-string references from DLL detach after Windows has already stopped workers and begun engine teardown. The browser host follows the game-process handle and exits independently, while in-session browser-host recovery synchronizes and cancels pipe accept, reads, and writes before releasing handles, preventing exit hangs, late teardown faults, and I/O races.
- Browser-host IPC messages no longer write synchronously from Starfield's calling thread. A bounded background writer gives shutdown only 250 ms to reach the browser host before canceling the transport; bounded/coalescing queues prevent a stalled browser host from growing memory, and hello deadlines plus heartbeats replace a connected-but-frozen browser host automatically.
- Unexpected browser-host and OSF UI runtime exceptions are contained, and an incomplete Scaleform hook set is rolled back instead of leaving half of the draw protocol installed.
- Live key rebinding, virtual-cursor input, settings and hotkey unsubscription, settings persistence retries, and content-folder scans are hardened against cross-thread or filesystem failures that could previously lose a write, invoke released plugin state, or terminate the UI.
- Reveal handshakes now drain pre-reveal captures while atomically changing presentation epochs and use a fresh token for every open, so a queued transparent frame or a copied old sentinel cannot reveal the overlay.
- System Health no longer describes the retired Present-hook fallback; a missing UI seam is reported as unavailable instead.

### Other changes

- Which HUDs start with the game is now the player's choice: every eligible HUD row in Mod Settings gains a "Start automatically" switch (applies at the next launch). A HUD manifest's `openOnStart` is only the author default, choices persist outside shipped mod files (`OSFUI/state/view-policy.json`, kept even while a mod is temporarily uninstalled), hidden utility views (`hub:false`) can never auto-run in the background, and `debugOnly` views qualify only while developer mode is on.
- Views are instantiated on first use. Hidden document instances suspend after about 90 seconds of game time; non-pinned instances are reclaimed after 25 hidden minutes, or earlier past a cap of four hidden closed views (least recently used first — open or pinned views never count), and are recreated on their next open. Browser resource use follows views actually used during the session.
- Removed the unsupported CPU mock-renderer override and the null renderer/compositor fault-isolation paths. Production uses the WebView2 web renderer and D3D12 compositor.

### Security

- WebView2 network isolation now fails closed when the required request filters or policy script cannot be installed. Untrusted page messages are capped at 64 KiB and 128 per second per view, and bridge-disabled views no longer forward arbitrary page traffic to the game process.
- The private browser-host IPC pipe is now created before the browser host launches, and both processes verify the peer PID reported by Windows before trusting protocol messages or retaining process handles. This closes the pipe-name impersonation window even for another process running as the same user.

### For plugin authors

- ABI 1.0–1.8 binaries temporarily receive a frozen ABI 1.8 adapter instead of `nullptr`. It preserves settings, hotkeys, view registration, health reporting, retained state, registered requests, and the old `RegisterCommand` request-ID/auto-ack path while warning once per DLL that support ends in 2.1.0. ABI 2.x remains strict and unrelated majors are still refused.
- Native ABI **2.0** makes `SetViewState(modId, key, payloadJson)` part of the baseline and replaces ambiguous commands with strict sends. Set retained state once and the OSF UI runtime replays it to every document of that mod; use `RegisterSend` for one-way messages and `RegisterRequest` plus `Request::Respond` / `Reject` for work requiring an outcome. The optional JSON facade now exposes `JsonRequest::Name()` for that request endpoint while retaining `Command()` as a source-compatible alias for the frozen ABI field.
- `RegisterView` now validates ordinary plugin-shipped views without eagerly instantiating them; an explicit `RegisterView` still honors manifest `openOnStart` immediately (menus included — that path is plugin opt-in, unlike discovery, which never auto-starts menus). `SendToWeb` keeps a bounded FIFO holdback for known discovered-but-uninstantiated or reclaimed targets, so sending an initial event immediately before opening a view retains the existing first-paint ordering guarantee. The per-view 64-message drop-oldest bound now applies to every target — including instantiated views, which were previously unbounded between ticks — matching the renderer's own per-view queue bound; on overflow, the newest happenings are retained for delivery.
- A user-rebindable `type: "key"` setting can now declare an immutable `onPress` GLOBAL Papyrus target. OSF UI queues it only after the normal gameplay hotkey gates, so a mod can start its quest or other script logic on demand without an always-running bootstrap quest or a load-order-dependent FormID in JSON; ordinary web, native, and registered Papyrus hotkey notifications still fire.
- Browser-host IPC protocol version 5 added best-effort suspension for idle hidden views along with verified peers, heartbeat liveness, and presentation epochs. The OSF UI runtime and browser-host binaries must be updated together.
- A deployable Papyrus-only example now demonstrates `onPress` with no `.esp`, startup quest, or callback registration, including rebinding, save-load, suppression, and failure-reporting checks.
- UnsubscribeSettings, UnsubscribeHotkey, and ready-callback replacement now wait for an already-running callback even when called off the main thread; once they return, the old user pointer is no longer in use. Self-unsubscribe remains supported.

### For view authors

- The `@osfui/cli` authoring harness mirrors the strict 2.0 API for current projects. During 2.0.x, an explicitly pre-2.0 target instead selects the same guarded 1.x façade as the game and prints the 2.1.0 removal warning; newly scaffolded projects still use only 2.0.
- Web bridge protocol **2.0**. The handshake is page-initiated and is the only boot path: the shipped shared bridge helper sends `osfui.hello` on every document and the OSF UI runtime answers `ready`, replays every current state value, then opens the event stream. First open, F5, developer-mode hot reload, and crash recovery are now literally the same sequence, so a correct view has no lifecycle code at all. Events emitted before a document greets the bridge are held in a bounded per-view queue (64, drop-oldest) rather than lost; state is not queued because the replay already covers it.
- Errors are typed, settle exactly once, and are loud where you are actually looking. `request()` rejects with a stable `code` — `no-bridge`, `timeout` (your 10-second client timer), `no-response` (a mod backend or OSF UI runtime handler missed the OSF UI runtime's 30-second deadline), `wrong-endpoint-kind`, `unknown-endpoint`, `invalid-request`, `request-capacity`, or the handler's own — and the shared bridge helper prints every rejection to the page console with an `[osfui]` prefix, so F12 Chromium DevTools shows it with full object inspection instead of it living only in a log file. View-caused faults the page would otherwise never hear about (a `send` that named a request endpoint, an unknown endpoint, or a malformed envelope) come back on a developer-mode-only `osfui.debug.error` event and print the same way. A handler timeout is reported as `no-response` but never counts against the calling view; in release builds, repeated view-caused misuse raises a **view.protocol-misuse** health issue.
- `localStorage["osfui:trace"] = "1"` (then reload) logs every envelope in both directions via `console.debug`, including request settlement latency. This is deliberately not a bespoke traffic inspector: DevTools already does filtering, object inspection, and preserve-log better than one could, per view, at zero cost when the flag is off.
- Papyrus gained `SendViewEvent(mod, name, args)`, arriving at `osfui.on("<mod>.<name>")` with `payload.args`. Without it, removing the transient push family would have left Papyrus with no event channel at all, and authors would have encoded one-shot happenings as state — which replays on reload and re-fires the "event".
- The `osfui/views` state key (which replaces `views.data`) carries `autoStart`, `autoStartMutable`, and `pinned`, and the platform-private `osfui.setViewAutoStart` request (Mod Settings only) persists a HUD's next-launch choice. Manifest `openOnStart` is now documented as the HUD author default; for discovered menus it is ignored.
- Hidden document instances may stop JavaScript timers after about 90 seconds, and non-pinned views may be reclaimed and receive a fresh document instance after long idle periods or when more than four closed views sit hidden. Use `ui.visibility` as the visit boundary, and state — `SetView*` from Papyrus, `SetViewState` from a plugin — for anything that must survive recreation. One-shot events are never replayed to a later document: Papyrus events with no instantiated target are dropped, while native `SendToWeb` has only a bounded pre-instantiation holdback for open ordering.
- A `type: "action"` row in a settings schema now targets your mod's own **request** endpoint. Respond with an object (an optional `message` string becomes a toast) or reject with a code; there is no `ok:false` document to inspect and no ack convention to remember.

## 1.5.0 — 2026-07-29

### Highlights

- `devMode` now reloads saved view files in game and supports **F12** Edge DevTools. New views and `manifest.json` changes still require a restart.
- `npm create osfui@latest` now scaffolds TypeScript menu/HUD projects with Papyrus or native backends, a Vite harness, validation, MO2 sync, and packaging.

### Added

- Settings schemas can define tabbed `pages`; unassigned groups appear under **General**.
- Added a consent-based **Report a bug** flow with local redaction, bounded log attachments, abuse limits, and private 30-day storage.
- Native plugins can publish self-clearing issues to the shared **System Health** pane.

### Fixed

- Native WebView2 form popups - including settings dropdowns, datalists, and date, time, or colour pickers authored by third-party views — now temporarily receive physical pointer ownership while open, including when the control lives inside an embedded game or panel. Their options can therefore be clicked in game instead of requiring keyboard selection, without view authors replacing standard HTML controls.
- MO2 hot reload now updates each deployed mod view tree in place and copies every replacement into the browser mirror before pruning old bundles, preventing USVFS path failures from disconnecting the current page and avoiding `ERR_FILE_NOT_FOUND` for renamed shared bundles.
- `osfui dev --game` now syncs view assets while Starfield holds native files open, without replacing a live directory that the game has already enumerated.
- Mouse-wheel input now uses the live cursor position and a consistent scroll distance.
- A quick left-stick navigation flick now moves focus exactly once instead of skipping over controls; deliberate holds still repeat after a longer pause, while release jitter and diagonal input no longer create extra steps.
- Removed swap-chain Present hooks that conflicted with OptiScaler, Steam Overlay, ReShade, RTSS, and similar tools.
- WebView2 composition-controller failures now close the loading overlay, restore input, and show repair guidance.
- Action buttons in a mod's settings no longer report a false **No response from &lt;mod&gt;** after working correctly. A plugin that acts on a button without sending its own reply — the documented minimum — was left waiting for the full timeout.
- Closed a rare instability when a mod registered a view after the overlay had already drawn a frame; the capture path could read the view list while it was being rebuilt.
- Keybind rows in non-English languages now show a translated **Gameplay** badge instead of leaving one of the two badges in English.
- Retained data pushed to a view no longer keeps a second copy of the whole payload in memory, which was noticeable for large lists such as a full inventory.
- Closing the Mods surface with the mouse or controller while a key rebind was waiting for input now cancels the rebind. Previously the next gameplay keypress was swallowed and silently committed as the new binding.
- Reopening the Mods surface after closing it with the undo panel open no longer leaves LB/RB category switching unresponsive.
- Overlay gamepad navigation now follows the controller the player is actually using. With a second device connected — a charging pad, a wheel, or a virtual device — it previously read only the lowest slot and went dead.
- A view opened on demand can now hide and show itself with `setViewHidden`; the command previously refused any view not preloaded at startup.

### Security

- Diagnostic uploads now require a default-deny native confirmation even if the packaged settings view is replaced, redact both slash forms of private roots, and keep accepted reports private until an administrator reviews them for publication.
- A view declaring `bridge: false` is now genuinely denied the `window.osfui` bridge; the injection previously happened anyway and only the game side dropped its messages.
- Blocked outbound requests are logged at most 32 origins per view and then go silent, so a page cannot grow the logs or hitch the frame rate by requesting an endless series of hostnames.
- Links now open in the system browser only from a real click. A scripted `window.open` — which never issues a request the network filter could see — is dropped, closing a data-exfiltration channel around the default-deny egress policy.

### For plugin authors

- Added optional `OSFUI_JSON.h` helpers for typed JSON commands, requests, replies, state, schemas, and diagnostics.
- Native ABI **1.7** adds asynchronous request/reply support with 30-second timeouts and 64 in-flight requests per view, plus namespaced `ReportIssue`, `ClearIssue`, and `ClearIssuesExcept` diagnostics APIs.

### Other changes

- Logs now keep the previous session, use full dates, tag third-party content errors, and reduce routine trace noise.
- Diagnostics now lists and can trigger every discovered third-party view.

### For view authors

- Generated Papyrus projects now include concise setup documentation, Spriggit source, ESM/PEX builds, and prerequisite checks.
- Generated assets now stay under `views/<modId>/assets/`; dependent mods no longer copy OSF UI's shared kit.
- Generated HUD projects now provide passive telemetry, persistence, settings, hotkeys, and browser mocks.
- CLI output paths are checked for unsafe overlap, development sync removes stale files, and malformed options fail early.
- `dev:game --deploy` now accepts an MO2 `mods` directory and creates the project mod folder.
- Papyrus and native presets now include matching, buildable backends and package all mod files from `mod/`.
- The project creator now uses guided prompts, safe defaults, local CLI linking, and matching JavaScript/TypeScript starters.
- Bridge protocol **1.5** adds qualified native request/reply behavior, `emit`, `call`, typed events, cached Papyrus state, actions and request/reply helpers.
- The author dev server no longer serves files from outside the project — private keys, `.env` files and certificates next to a project were reachable — and no longer resolves an absolute path out of a mod's asset route.
- `osfui build` refuses to delete a non-empty output directory it cannot prove it wrote, so an `outDir` pointing outside the project can no longer wipe a sibling's output.
- `osfui dev:game` now merges its answers into `.osfui/local.json` instead of overwriting it, and reports a malformed file rather than silently replacing it.
- `npm create osfui` rejects unknown or misspelled flags instead of scaffolding the wrong surface, and enforces the 64-character mod id limit the runtime applies.
- A mod id beginning with a digit now scaffolds a Papyrus script name the Creation Kit compiler accepts; such projects previously could not build at all.
- The pre-build content check now targets real outbound requests, so an inline SVG namespace or a link in view text no longer fails the build.
- A `manifest.json` written as TypeScript or JavaScript config no longer drops `accent`, and `icon` no longer produces a spurious unknown-key warning in `devMode`.
- A settings schema declaring a page id starting with an underscore no longer collides with the implicit **General** tab; page ids must now begin with a letter or digit.
- With `devMode` on, a newly instantiated view is picked up for file reloads immediately instead of after the next scan interval.
- Scaffolded HUD projects now toggle visibility through `hud.show`/`hud.hide` instead of `setViewHidden`, whose effect the menu policy undid whenever the player closed the Mods surface; the authoring docs now explain the difference.

## 1.4.0 — 2026-07-24

Added System Health and restored stable main-thread and compositor behavior.

### Added

- Added **System Health** with actionable issues, per-mod markers, resolved history, safe recovery actions, and copyable diagnostics.

### Fixed

- Restored the stable Scaleform compositor and prevented one key press from immediately reopening or closing the menu.
- Moved per-frame runtime work and native plugin callbacks back to Starfield's main thread.

### For view authors

- Bridge protocol **1.4** adds `diagnostics.get`, `diagnostics.data`, and `osfui.openLogFolder`.

## 1.3.0 — 2026-07-22

Added richer Papyrus data, reliable interactive focus, and developer diagnostics.

### Added

- Bridge protocol **1.3** adds `PushFormsToView`, `GetFormById`, and `GetFormsById` for session-scoped game forms.
- View actions can send argument arrays through `RegisterForViewActionsArgs`.

### Fixed

- Fixed mouse-wheel input after focus moves to WebView2.
- Fixed a stack-overflow crash with BetterConsole.
- The WebView2 helper no longer backgrounds Starfield during launch.
- Interactive views now receive native keyboard/IME focus while gamepads continue through XInput.
- Fixed crosshair cursors appearing as a loading spinner.
- Prewarmed the first-load transition.
- Fixed pause-menu close/reopen races and stale callbacks.
- Fixed a startup race in Scaleform hooks and removed `uiPassProbe`.
- Failed settings writes now preserve the previous file.
- Invalid host messages are dropped instead of crashing the game.
- Fixed synthetic key input and stale text-field focus.

### Other changes

- Added **Debug mode** for showing developer-only views.

### For view authors

- Manifests can set `"debugOnly": true`.

## 1.2.1 — 2026-07-22

Improved first-time loading and Frame Generation support.

### Fixed

- WebView2 now uses Starfield's GPU, fixing invisible menus on hybrid-GPU systems.
- Drop-in views can open on demand without config edits or a companion plugin.
- AMD FSR3 Frame Generation now composites the overlay through Starfield's UI layer.

### Other changes

- First-time loads now show a themed loading panel with retry and cancel actions.
- Disabled normal `uiPassProbe` capture work.

### For view authors

- Bridge protocol **1.2** adds manifest `accent`, `readySignal`, and `osfui.viewReady()`.

## 1.2.0 — 2026-07-21

Restored controller input, enforced network isolation, and improved renderer recovery.

### Fixed

- Controller navigation now resumes after text entry.
- Menu gamepad input no longer reaches gameplay.
- Crashed or hung views now recover or close without trapping input.
- Fixed a pause-menu rebuild crash.
- Reduced crashes with Frame Generation by skipping generated swap chains.
- Restored visible focus styling without stealing game input.

### Security

- Views can no longer access the network or use WebSocket, WebRTC, WebTransport, Worker, or SharedWorker. External links still open in the system browser.

### Other changes

- Missing WebView2 Runtime now shows an installer prompt.
- `devMode` now mirrors browser console output to `OSF UI.log`.
- Expanded the shared-texture ring to tolerate brief game stalls.
- Coalesced high-frequency mouse movement to one host update per game frame.

## 1.1.1 — 2026-07-20

### Fixed

- Elevated game sessions now launch an elevated WebView2 host, fixing invisible overlays.

### Other changes

- Added a separate WebView2 host log beside `OSF UI.log`.
- Host startup errors now include useful diagnostics and clear Mark-of-the-Web from the mirrored executable.

## 1.1.0 — 2026-07-20

Moved rendering to WebView2 and added Papyrus-driven live data.

### Highlights

- Replaced Ultralight with an out-of-process Microsoft Edge WebView2 renderer.
- Added `PushToView` data and `ui.action` callbacks for Papyrus mods.

### Fixed

- Mod hotkeys no longer fire while typing in the game console.
- OEM punctuation keys are now bindable.
- Fixed rare input loss after closing the Mods menu.

### For view authors

- Added `osfui.openModPage`.

### Other changes

- **Needs update** links now open the OSF UI Nexus page.
- Removed the Ultralight backend and packaging path.
- Release builds now install and verify the WebView2 host.
- Built-in views now use Vite, TypeScript, and Preact.

## 1.0.0 — 2026-07-17

Initial release.

### Highlights

- Added HTML/CSS/JavaScript views inside Starfield.
- Added the **F10** Mods surface and visual keybinding editor.
- Added keyboard and controller navigation.

### For view authors

- Added manifest-based view packages with multi-view composition.
- Added `targetVersion` compatibility notices.
- Added JavaScript bridge protocol **1.0**.
- Added persistent, versioned settings schemas.
- Added shared CSS and TypeScript definitions.
- Added `devMode`, **F11** reload, schema reload, and a browser harness.

### For plugin authors

- Added the native C++ API for registering views and handling commands.

### Engine integration

- Views use the game menu stack, pause state, input controls, and hardware cursor.
- User settings persist separately from the shipped developer config.
- Ultralight rendering was an opt-in build.
