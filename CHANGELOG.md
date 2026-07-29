# Changelog

## Unreleased

### Highlights

- Added the native first slice for an opt-in in-world Web UI surface, now productionized to multiple screens: up to four `worldSurfaces` config entries each run their own view in an isolated WebView2 host and shared-texture ring, stay alive while the fullscreen overlay is closed, survive engine descriptor rebuilds, and report host failures and degradations into System Health without affecting the overlay or each other. Placeholder textures are build-generated with validated collision-safe sizes, and `docs/world-surface-authoring.md` documents the Creation Kit recipe (material, mesh, placeable) an in-world screen needs. No world material assets ship yet — the loose cockpit-material experiment proved unsafe and was removed — so the feature stays behind the `with_world_surfaces` build flag until a CK-authored screen passes in-game verification. Display-only for now; interaction remains follow-up work.

- In **devMode**, loose view edits now appear in game without rebuilding or restarting: save HTML, JavaScript, CSS, or a local asset and OSF UI refreshes the loaded view after the file settles. Polling and Mod Organizer 2 mirror synchronization run in the background so large view folders do not stall the game; removed or renamed files are deleted from the browser mirror instead of lingering, and failed copies retry with backoff. F11 refreshes the mirror too. `manifest.json` changes and newly added views still require a restart.

- View authors can press **F12** in `devMode` to open Edge DevTools for the top open menu, making its live DOM, styles, blocked requests, and JavaScript state inspectable without leaving the in-game iteration loop. The browser DevTools capability remains disabled outside `devMode`.

- View authoring is now a standalone npm workflow instead of a repository-only harness: `npm create osfui@latest` scaffolds dependency-free TypeScript or JavaScript projects with menu/HUD and Papyrus/native/settings/static starting points, and `npm run dev` opens a production-shaped Vite harness with HMR, shared UI assets, mock state/requests, lifecycle controls and bridge inspection. `dev:game` builds and syncs the view into an author-selected MO2 mod while enabling an expiring, session-local author mode—so F11 reload and F12 DevTools require no permanent player configuration—and `check`, `build`, `doctor` and `package` cover validation through a release-ready zip. The lower-level packaged-view harness remains available for diagnostics.

### Added

- **Settings schemas can segment into pages.** A mod with many settings can declare top-level `pages` and tag each group with a `page` id; the Mods surface then renders a tab row and shows one page at a time instead of a single long column. Untagged groups collect on an implicit **General** tab painted first, unreferenced pages render no tab, and cross-mod search jumps raise the owning tab before flashing the row. Pages are display-only annotations on the flat `groups` list, so the same schema renders as the familiar one-column list on hosts that predate them — no native or ABI change involved. Group collapse state and section-index anchors now prefer a group's stable `id` over its array position and label, so schema reorderings and repeated labels across pages no longer confuse them. See `docs/authoring-settings.md` §3.

- Maintainers can now open the reporting service's private `/admin` dashboard, unlock it with the existing admin token, browse submitted reports, and inspect each player's consented diagnostics and log artifacts without retrieving report IDs one at a time. Listing and detail requests remain authenticated, reporter content is rendered only as text, and the token is retained only for the browser tab.

- System Health now has a consented **Report a bug** flow that accepts a private report reference and queues public GitHub issue creation after server-side abuse checks. It attaches bounded, locally redacted tails of both OSF UI logs plus the current health snapshot; the disclosure names every included file, the potentially public title/description/reproduction fields, and the 30-day retention period. Known account and install roots are removed on the PC, logs never enter the public issue, and a failed or unconfigured service leaves the existing copy/open-log fallback intact. Installations use renewable signed tickets; the service combines edge throttles with globally consistent daily installation, network, and total budgets, neutralizes public mentions/HTML, serializes GitHub publication through a retrying queue, and provides independent intake/publication pause switches. When Starfield exits with a non-zero process status while the interactive OSF UI is active or opening, the surviving primary WebView2 helper offers the same private upload in a Windows Yes/No dialog without claiming OSF UI caused the crash. If the active surface belongs to OSF Animation, the disclosure names that repository and, after consent, the helper attaches the session's exact `OSF Animation.log`; the service routes the public issue to `ozooma10/osf-animation` through a fixed server allowlist rather than accepting an arbitrary repository from the client. Only after consent does the helper also look in the standard `SFSE/Crashlogs` folder for the newest Trainwreck/Crash Logger report created during that session. All attachments use path-free names, local redaction and bounded tails. The helper resolves the game's exit status even when a crash closes the IPC pipe first, while suppressing intentional non-zero exits such as the `qqq` console command when OSF UI is inactive. Player-initiated closes — the taskbar's **Close window**, the title-bar X, Alt+F4, or logging off — are also suppressed: the in-game hook reports the close request to the helper the moment the game window receives it, because Starfield's forced teardown routinely ends with a non-zero (access-violation) status even though nothing went wrong.

- **System Health is now the whole game's health pane, not just OSF UI's.** Any mod built on the native API can report a problem into it — a pack that failed to parse, a missing asset, a feature it had to switch off — so you look in one place when something is wrong instead of having to know which mod noticed first. Reports name the mod that made them, clear themselves when the condition goes away, and appear in **Copy diagnostic report** alongside everything else.

### Fixed

- Mouse-wheel scrolling in the interactive UI now scrolls whatever sits under the visible cursor and moves a consistent distance everywhere. Wheel input used to be injected at a cached pointer position that could go stale and park in a screen corner — leaving scrolling dead or seemingly tied to which element was hovered — and the embedded browser's percent-based scrolling additionally made the distance per notch depend on the height of the hovered scroll area. The wheel now samples the live cursor position at injection time, and percent-based scrolling is disabled while the smooth scroll animation is kept.

- Normal builds and release packages no longer carry in-world screen research assets. A Material Editor Lite-authored `OSFUI_WorldScreen01.mat` could make every other world material disappear even with the OSF UI DLL absent, and switching the research flag off previously left that material and generated placeholder textures behind in an existing MO2 deployment. The unsafe material has been removed; production builds now exclude these assets and purge stale research copies when redeployed.

- OSF UI now boots its real WebView2 renderer, D3D12 compositor, and UI input hook from compiled production defaults when the developer config omits backend selections. F10 therefore opens the UI again, and choosing **MOD MENUS** from the pause menu no longer pauses behind an invisible mock frame after an update replaces `config.json`.

- The Mods surface no longer opens with an empty **All systems** screen when MO2's browser mirror retains an older shared bridge helper. Each game process now builds views into a fresh real-path mirror and removes it after the browser exits, so current view bundles cannot accidentally run against stale `osfui.emit()` / `osfui.call()` support. Catalog reads are also retried when the browser transport becomes ready.

- Fixed the reproducible DXGI crash when OSF UI, OptiScaler/Streamline, and the Steam overlay were active together. The compositor now stays entirely in Starfield's Scaleform UI seam and never hooks the swap-chain Present path, so frame generation and external overlays can remain enabled without load-order or configuration workarounds; the same isolation also avoids probe/hook conflicts with BetterConsole, RTSS, ReShade, and similar tools.

- If WebView2 fails while creating its composition controller, OSF UI now closes the invisible loading overlay immediately and releases focus, pause, cursor, and control capture instead of leaving actors frozen behind a hidden menu. A native dialog identifies the renderer error and offers Microsoft's WebView2 repair download; further menu opens fail closed until the game is restarted.

### For plugin authors

- Native plugins that already use `nlohmann::json` can include the new optional `OSFUI_JSON.h` facade instead of hand-authoring and parsing JSON strings. `JsonCommand` and `JsonRequest` provide typed field/struct access, malformed requests reject automatically, replies serialize safely, and `JsonClient` accepts JSON values for outbound messages, runtime schemas and diagnostics. The dependency-free `OSFUI_API.h` ABI remains unchanged at 1.8; the facade compiles entirely into the consuming plugin.
- Native ABI **1.8** (`Feature::kRequests`) adds first-class request/response beside fire-and-forget commands. A plugin registers a qualified request name, receives a copyable respond-once token, and may answer later from any thread without seeing or echoing a `requestId`; the host routes the plugin-owned reply type and payload to the calling view. Tokens time out after 30 seconds with `no-response`, are reaped when the view closes, and become safe no-ops after settlement. Requests share commands' first-wins namespace and are capped at 64 in flight per view. Plugins can reject with their own stable error code through correlated `ui.error`; older hosts safely no-op the new `Client` methods.

- Native ABI **1.7** (`Feature::kDiagnostics`) adds `ReportIssue`, `ClearIssue` and `ClearIssuesExcept` to `IOSFUIBridge`/`Client`: raise a durable condition into System Health, withdraw it when it clears, or reconcile a recomputed set in one sweep. Issues are identity-keyed (a repeat bumps an occurrence count rather than stacking a card), and the `source`, id and code are namespaced to the calling mod **by the host**, so no mod can file a report against another or resolve a platform issue. `context` is a flat JSON object, bounded and path-sanitized. A code OSF UI does not recognise renders as a card naming your mod with your details attached — never a blank one. On a host older than 1.7 all three are no-ops returning false. See §5d of [native-plugin-api.md](docs/native-plugin-api.md), including what does *not* belong in the pane.

### Other changes

- The in-world render-to-texture prototype is excluded from normal builds and releases. Its descriptor hook, development probe, second browser host, configuration keys, runtime state, and generated placeholder textures are enabled only with the default-off `with_world_surfaces` research flag. Unsafe loose material experiments are not shipped, and returning to a normal build purges research assets left in an existing deployment.
- OSF UI → Diagnostics now has a collapsed **Registered views** menu listing every mod-provided view discovered during the session, including hidden and not-yet-loaded views, with a **Trigger** button that sends it through the normal open path. Built-in OSF UI surfaces stay out of the list, keeping it focused on confirming and manually launching mod diagnostic or utility views without exposing them in the regular mod menu.

### For view authors

- The Papyrus, native-plugin, and settings choices in `npm create osfui@latest` now scaffold the matching backend instead of changing only the browser demo: Papyrus projects include a request-listener script and Creation Kit steps, settings projects include a working schema, and native projects include a CommonLibSF/xmake plugin with both native API headers. The native preset is a paired C++/web showcase built on `OSFUI_JSON`: typed fire-and-forget commands, correlated requests and replies, C++ state pushes, ready/settings/hotkey callbacks, a runtime settings schema, and a stateful browser mock all work out of the box. Generated projects use `mod/` as their Starfield Data-root tree; `build`, `package`, and `dev:game` carry those scripts, DLLs, schemas, and other mod files alongside the compiled view.

- `npm create osfui@latest` now walks through setup like Vite: language, menu/HUD surface, and starting workflow are shown as concise arrow-key selection lists instead of asking authors to type hidden option names. Directory name and view ID are short text steps with editable defaults, while mod ID shows the expected format but requires an explicit entry. This lets a command run from a populated folder create a safe child project instead of trying to overwrite that folder. Explicit flags and directory arguments continue to work for automation. When the creator itself runs from the OSF UI repository, generated projects now link the sibling `@osfui/cli` package instead of requesting its not-yet-published version from npm; Windows dependency installation also avoids Node's insecure-shell deprecation warning, and the generated TypeScript and JavaScript starters use the same framework-free DOM example, with strict checking enabled for the TypeScript choice.

- Bridge protocol **1.7** declares the built-in reporter's correlated status/result messages and commands. They are platform-private: every caller except the exact `osfui/settings` view is rejected, endpoints remain host-owned HTTPS configuration, and third-party views still have no network access.

- Bridge protocol **1.6** adds a simpler event/state/request authoring layer without removing the raw bridge: `osfui.emit()` names one-way native commands, `osfui.call()` returns a correlated reply payload directly, and generic `osfui.on<T>()` improves custom message typing. Papyrus mods can publish naturally typed, session-cached `SetView*` state that automatically replays when a view opens or reloads and is consumed with `osfui.data.on()`—no `ready` action, key filtering or number-as-string conversion. `osfui.action()` plus `ListenForViewActions()` provide the concise one-way path, while `osfui.papyrus.request()` and the typed `ReplyView*`/`RejectViewRequest` natives add bounded, one-shot request/reply with host-owned correlation and timeout handling. Legacy `send`/`request`, `PushToView`, and custom callback registrations remain compatible. Protocol 1.5's qualified native-plugin requests also continue to ignore the old successful delivery ack and wait for the plugin's typed response.

## 1.4.0 — 2026-07-24

Mod Settings gains a **System Health** destination that turns silent, log-only failures into plain-language warnings with a clear next step. Underneath, OSF UI's per-frame runtime now runs on Starfield's main thread, and the Scaleform seam compositor is back on its known-good implementation.

### Added

- Mod Settings has a new **System Health** destination, pinned at the top of the rail. It shows durable, plain-language warnings and errors — a settings file that could not be read, a screen that failed to load, two OSF UI copies mixed together, or something installed that needs a newer OSF UI — each with what it means, what to do next, and safe actions like **Retry view**, **Open log folder** or **Update OSF UI**. A calm summary reads **All systems nominal**, **Warnings detected** or **Action required**; the rail badge and per-mod markers point you to what needs attention. Issues clear themselves when the underlying condition goes away and move to a **Resolved this session** list (cleared on exit) — nothing to dismiss by hand, and no log noise. A **Copy diagnostic report** button produces a paste-ready summary for bug reports. This replaces the old settings-load alert; failed launcher cards now read **FAILED — REVIEW ISSUE ▸** and link straight to the issue. Conditions that stop OSF UI from rendering at all still surface through the launch dialog and logs as before.

### Fixed

- The Scaleform seam compositor is restored to its known-good pre-`b8e3643` implementation. The attempted root-signature and pipeline-state interception for a rare black-HUD edge case could instead make Mod Settings disappear or crash the graphics driver, so those hooks and their fail-closed draw gating have been removed. Opening the menu while its toggle key is still held also ignores WebView2's cross-focus key repeat, so one press cannot immediately close the UI again.
- OSF UI's per-frame runtime now runs on Starfield's main thread even though SFSE supplies its frame notifications from render workers. Pause, free-cursor and control-layer bookkeeping no longer races the engine, in-game calendar reads are synchronized, and native plugin callbacks once again honor their documented main-thread contract.

### For view authors

- Bridge protocol 1.4 (additive, stable) adds the session-health channel behind System Health: `diagnostics.get` returns the current `{ system, issues }` snapshot and subscribes the caller to `diagnostics.data` change pushes. Each issue carries a stable machine `code`, `severity`, `status`, source/subject ids, bounded and path-free technical `context`, an occurrence count, and session-relative timings — player-facing wording is derived from the code in the built-in frontend, never sent on the wire. Also adds the payload-free, fixed-target `osfui.openLogFolder` command (the SFSE log directory) alongside the existing `osfui.openModPage`. A normal content view needs none of this; it powers the framework's own Mods surface. Declarations are in `sdk/osfui.d.ts` and `docs/authoring-views.md`.

## 1.3.0 — 2026-07-22

Papyrus scripts can now hand real game forms and multi-argument actions to their views across the bridge (protocol 1.3), interactive views get genuine keyboard/gamepad focus without backgrounding the game, and the pause-menu and render-seam paths are hardened against startup races and Scaleform faults. Adds a built-in Web Performance Lab and a Debug-mode switch for developer surfaces.

### Added

- Papyrus scripts can now push **real game forms** to their views (bridge protocol 1.3). `OSFUI.PushFormsToView(mod, key, Form[])` delivers each form to the page as a structured object — `{ formId, formType, name }` — inside the existing `data.push` message, so a view can render a dynamic list of keywords, items, or any other forms with zero FormID bookkeeping in JS. To act on one, the view simply echoes `formId` back as a normal action argument and the script resolves it with the new `OSFUI.GetFormById` / `GetFormsById` natives (which, unlike `Game.GetForm`, accept hex and the full 32-bit dynamic-FormID range). This replaces encoding forms as catalog indices and resolving them by position. `None` entries keep their slot as `null`, so a parallel `PushToView` of counts or labels stays index-aligned. References are session-scoped runtime FormIDs — resolve promptly, never persist them (see `docs/authoring-dynamic-data.md`, "Real forms across the bridge").
- View actions can now carry a **list of arguments** (bridge protocol 1.3). A view sends `osfui.send('ui.action', { action, args: [...] })` and a script registered with the new `OSFUI.RegisterForViewActionsArgs` receives them as `OnUIAction(string asAction, string[] asArgs)`. This replaces the long-standing workaround of packing several small ints into one string (`kind*100+slot`) to squeeze through the single-string `arg` channel — Papyrus has neither a modulo operator nor substring parsing, which made unpacking awkward. The original scalar `arg` / `RegisterForViewActions` shape is unchanged, and both can be used by the same mod at once.

### Fixed

- Mouse-wheel scrolling now works in interactive UI pages after browser focus moves into the WebView2 helper. The helper captures wheel packets on its own window instead of depending on subclassing Chromium's focused child window across a process boundary, while retaining the previous routes as fallbacks.
- Fixed a crash when OSF UI and BetterConsole are enabled together. If BetterConsole re-hooks the game window after OSF UI, the two input hooks no longer chain back into each other until the process exhausts its stack; both overlays still receive input and Starfield's own window procedure remains at the end of the chain.
- Starting Starfield with OSF UI enabled no longer lets the WebView2 helper's offscreen bootstrap window take foreground activation and leave the game backgrounded during launch.
- Interactive menus now give their WebView genuine foreground focus for the whole session, matching the smooth behavior observed when Starfield loses focus without overriding either process's GPU priority. Keyboard and IME input route natively, mouse movement/buttons/wheel are captured by the host, and XInput gamepads keep the existing navigation/raw-event behavior even though Starfield's focus-gated controller feed pauses. HUD-only views keep Starfield focused and cap capture at 60 Hz to bound gameplay GPU/copy pressure; interactive capture can run up to 240 Hz on supported Windows builds. Chromium also no longer treats the tiny offscreen capture host as occluded background work, while explicitly hidden views still suspend normally.
- Fixed CSS crosshair cursors appearing as the Windows loading spinner, most visibly over the Web Performance Lab's animation stage.
- The first-load transition is now painted once during startup while it is still hidden, so invoking it no longer waits for a cold WebView renderer before it can appear.
- Hardened the Pause-menu “MOD MENUS” entry against close/reopen races: periodic injection now runs only while the engine-admitted movie is advancing, and stale click callbacks are swallowed without opening the overlay or forwarding OSF UI's private action id into the game. Unexpected Scaleform faults are also left to the crash logger instead of being caught after the VM is already corrupted, which previously turned the crash into a hang.
- Closed a startup race in the Scaleform UI-seam hooks: the overlay's D3D12 command-list hooks now publish the engine's original function *before* the hook becomes reachable, eliminating a narrow window where a render thread could route through a half-installed hook, drop a GPU call or skip a UI pass, and fault the game. The `uiPassProbe` render-target characterization diagnostic — its capture hook, capture windows, and per-pass logging — has now been removed entirely; only the seam's load-bearing hooks remain, so no diagnostic code sits on a hot engine code path.
- Hardened settings persistence: if writing a settings file fails partway (disk full or I/O error), OSF UI now keeps your existing values instead of replacing them with a truncated file that would be quarantined and reset to defaults on the next load.
- A malformed, truncated, or version-mismatched message from the WebView2 host is now dropped with a warning instead of throwing out of the host-reader thread, which previously crashed the whole game to desktop.
- Fixed synthetic key presses (gamepad-driven typing, and the Space key) being silently dropped inside views: a leftover reference from the focus rework threw before the key was dispatched. Text fields also blur correctly again when a view is hidden.

### Other changes

- Added a built-in **Web Performance Lab**: repeatable paint, transform, DOM layout, Canvas 2D, CSS effects and mixed-scene workloads run without pausing the game; it records RAF cadence, frame-time percentiles, JavaScript work, timer jitter and input-to-RAF delay, and can run or copy a complete reference suite for comparing machines and OSF UI builds. It is a developer tool, so it no longer appears in the mod menu during normal play — turn on **Debug mode** (below) to reveal it.
- Mod Settings has a new **Debug mode** switch under OSF UI → Diagnostics (off by default). When on, developer views such as the Web Performance Lab are listed in the mod menu; off keeps them hidden. Normal play is unaffected.
- Mod Settings now has one persistent **Show render stats** switch under OSF UI → Diagnostics. Its primary **Fresh view** rate measures new WebView textures that actually reach the game's compositor, rather than browser animation callbacks. The panel and periodic OSF UI logs also separate capture, transport, internal overlay-pass and present cadence; report capture-to-draw and compositor CPU time; count reused frames, stalls, waits and drops; and sample the actual animated document inside same-origin iframes instead of misreporting a static launcher. It applies to every view, including views loaded after it is enabled.

### For view authors

- A view manifest can set `"debugOnly": true` to keep the view out of the mod menu unless the player enables **Debug mode** (OSF UI → Diagnostics). The view still loads and can be opened by id — useful for a mod's own diagnostic/developer surfaces.

## 1.2.1 — 2026-07-22

Frame Generation now carries the overlay on both real and generated frames, and first-time mod menus open cleanly without configuration edits or blank loading surfaces.

### Fixed

- Fixed the Mods menu opening as a paused, cursor-active but invisible overlay on hybrid-GPU systems. The WebView2 host now renders on Starfield's actual GPU instead of whichever adapter Windows assigns the helper process, so its shared frames can be composited; failures also report the exact HRESULT and both adapter identities.
- Drop-in views can now be opened without editing the user's `config.json` or shipping a companion SFSE plugin: `menu.open`, Papyrus `OSFUI.OpenMenu`, and the native `RequestMenu` API load a discovered `views/<modId>/<viewName>/` folder on first use. Missing ids are rejected synchronously, so Papyrus and native callers can reliably fall back instead of receiving success for an open that the runtime later ignored; Papyrus view ids are also matched correctly when `BSFixedString` interning changes their letter casing.
- Starfield's built-in AMD FSR3 Frame Generation is now fully usable with the overlay. OSF UI records into the game's transparent UI layer instead of drawing into FG-owned present chains, so opaque and translucent content stays stable across real and generated frames, through loading, rapid mouse repaint, FG toggles, and display-mode changes. The old present path also no longer retains swapchains and remains as a fail-closed fallback that suspends itself under FG if the UI seam cannot be installed.

### Other changes

- First-time menu opens now stay in-world while their WebView starts: quick loads appear directly, while slower ones use an always-warm local-link panel carrying the destination's title, accent, and input/pause behavior. Broken or never-ready views offer retry/cancel instead of exposing a blank input-capturing screen; subsequent opens remain immediate.
- `uiPassProbe` remains an off-by-default compatibility diagnostic for the Scaleform seam. Normal seam rendering no longer runs its capture windows, object scans, or characterization logging.

### For view authors

- Bridge protocol 1.2 adds optional manifest `accent` and `readySignal` fields plus `osfui.viewReady()`: views that need initial async or Papyrus data can now choose their meaningful first-paint milestone, and OSF UI holds the diegetic handoff until they report it.

## 1.2.0 — 2026-07-21

Controller play works properly again, views are cut off from the network for real, and the overlay now survives renderer crashes and game stutters. (This release also includes everything from the unpublished 1.1.2.)

### Fixed

- Controller support works again. The WebView2 renderer kept Windows keyboard focus in the browser for the whole overlay session, and Windows only delivers gamepad input to the process whose window has focus — so the game engine (and the overlay with it) went controller-deaf. The overlay now leaves focus with the game and only moves it into the browser while you actually type in a text field (click a field or start typing); controller navigation resumes the moment text entry ends.
- With a menu open, gamepad input no longer leaks into the game underneath: the thumbsticks walked the player around (and buttons could trigger game actions) behind the overlay, because the engine's control-disable flags don't gate thumbstick movement. Gamepad events are now consumed at the overlay's input receiver while a capturing menu is open, so the game never sees them; views still receive them normally (default navigation mapping and raw `ui.gamepad` alike).
- A crashed or hung view no longer strands a blank overlay that still swallows input. When a view's browser render process exits or becomes unresponsive, the host now reports it and the runtime retries the load with backoff, then cleanly removes the view if it keeps failing; a total browser-process loss hides the overlay for the rest of the session (with the cause logged) instead of leaving a dead host the game still believes is alive.
- Fixed a crash tied to the pause-menu entry while the menu list was rebuilding.
- Likely fix for crashes with Frame Generation enabled: overlay drawing is now gated to real presents and skips FG-paced swapchains. If you crashed with FG on, please try again and report.
- Keyboard and gamepad focus is visible again, and clicking inside a view no longer briefly makes the game go input-deaf. Because focus now stays with the game so controllers keep working, the browser itself was never focused — so focus outlines and `:focus`/`:focus-visible` styling didn't render (navigation was working, but looked like nothing was happening), and a click landing on a focusable element could strand Windows focus in the browser process, cutting keyboard, mouse, and gamepad until a watchdog recovered it. The overlay now emulates page focus for styling without taking OS focus, and hands focus straight back to the game if a click grabs it.

### Security

- Views can no longer reach the network. OSF UI's no-network policy was declared but not actually enforced; the WebView2 host now denies every http(s) request whose origin isn't the local `osfui.local` view root — answering with a local 403 before anything leaves the machine, page navigations included — and removes the transport and worker APIs that could otherwise slip past a request filter: `WebSocket`, `RTCPeerConnection`/WebRTC, `WebTransport`, `Worker`, and `SharedWorker`. Views must bundle their assets locally; remote fonts, images, scripts, or analytics are blocked. `target="_blank"` links still open in the system browser as before. See `docs/security-model.md` (rule 2).

### Other changes

- If the Microsoft Edge WebView2 Runtime is not installed, a dialog now appears at game launch naming the problem and offering to open Microsoft's installer download — previously the overlay just never appeared, with the cause buried in `OSF UI.log`.
- In `devMode`, a view's `console.log` / `console.warn` / `console.error` output is now mirrored into `OSF UI.log` (at INFO / WARN / ERROR), so a misbehaving view is diagnosable in game rather than only in the browser harness. Off in normal play.
- The overlay rides out brief game stutters without dropping frames: the shared-texture ring between the WebView2 host and the game grew from 3 to 4 slots, so one slow game frame no longer stalls the host's capture thread (which showed up as skipped or late overlay frames under load). Costs one extra overlay-sized texture of VRAM (~8 MB at 1080p, ~33 MB at 4K).
- Moving the mouse over the overlay is now much cheaper: a high-polling-rate mouse (500–1000 Hz) was sending one cursor-update message to the WebView2 host per raw input packet — hundreds per second of pure overhead, since the page only samples the pointer at display refresh. Cursor moves are now coalesced to a single message per game frame carrying the latest position; clicks and scrolling are unaffected and still fire immediately. In `devMode` the log periodically reports how many packets were folded into how many sends.

## 1.1.1 — 2026-07-20

### Fixed

- Running the game/MO2 as administrator no longer leaves the overlay invisible: an elevated game now launches the WebView2 host elevated via the Task Scheduler, so the host can connect (thanks to the user who reported this!).

### Other changes

- The WebView2 host now logs to `Documents\My Games\Starfield\SFSE\Logs\OSF UI.webview2-host.log`, next to `OSF UI.log` — sharing that one folder covers both.
- When the host fails to start, `OSF UI.log` now explains why: host startup errors are forwarded into it, it embeds the host log's tail, and it reports whether the host exe survived (antivirus), carries Mark-of-the-Web (SmartScreen), or the game runs as administrator. Mark-of-the-Web is stripped from the mirrored host exe automatically.

## 1.1.0 — 2026-07-20

Views now render in Chromium, and Papyrus mods can drive them with live data.

### Highlights

- **New renderer: WebView2.** Views render in Microsoft Edge WebView2 inside a separate host process, replacing Ultralight.
- **Dynamic data for Papyrus mods.** Scripts can drive a view with live data and react to its clicks.
  `OSFUI.PushToView(modId, key, values)` delivers a string list to every loaded view of the mod as a `data.push` message; 
  views fire actions back with `osfui.send('ui.action', { action, arg })`, dispatched to `OSFUI.RegisterForViewActions(receiver, fn, modId)` / `...Static` callbacks (session-scoped,  released with `Unregister`). 
  Everything is fire-and-forget: Papyrus owns the data, views re-request state by firing a `ready` action on load, and OSF UI caches nothing. 
  See the new `docs/authoring-dynamic-data.md` (worked example: porting a terminal menu).

### Fixed

- Mod hotkeys no longer fire while typing in the game console.
- The OEM punctuation keys (`- = [ ] \ ; ' , . /`) are now bindable
- Input no longer dies after closing the Mods menu in rare cases 

### For view authors

- New `osfui.openModPage` command: opens OSF UI's Nexus page in the user's system browser, for "update OSF UI" affordances in views.

### Other changes

- The "Needs update" tag in the Mods menu now links to the OSF UI Nexus page; in game it opens in the default browser.
- The Ultralight backend, its SDK build option, runtime payload, and renderer-specific packaging path are gone.
- Release builds install and verify `OSFUI/bin/osfui_webview2_host.exe`.
- Internal: built-in views are generated from a Vite + TypeScript + Preact workspace under `frontend/`

## 1.0.0 — 2026-07-17

Initial release.

### Highlights

- **HTML/CSS/JS views over Starfield** - an SFSE/CommonLibSF plugin that renders web UI in game via the Ultralight engine. Inspired by Prisma UI.
- **Mods surface** - press **F10** (rebindable in game) to open the unified Mods menu, where OSF UI and content mods expose their settings.
- **Keybinds view** - a visual keyboard map with inline rebinding and conflict badges.
- **Controller support** - the Mods and Keybinds surfaces are fully navigable
  with a gamepad: D-pad / left stick moves focus, A activates, B closes,
  right stick scrolls, LB/RB switch mods. The same layer makes both surfaces
  arrow-key navigable from the keyboard.

### For view authors

- View packages under `views/<modId>/<viewName>/` with a `manifest.json`;
  multiple views can load and composite together (HUDs beneath open menus).
- `targetVersion` (view manifests AND settings schemas) — declare the OSF UI
  version a mod is authored against; when the installed OSF UI is older, the
  Mods surface shows a "needs update" badge by the version number naming the
  mod (advisory only — everything still loads best-effort).
- JS bridge `window.osfui`, **protocol 1.0** (stable): request envelope with
  `requestId` correlation, uniform `ui.result` outcomes, and raw gamepad
  events (experimental).
- Declarative **settings schemas** with persistence under
  `Documents\My Games\Starfield\OSFUI\settings\`, versioning + migration,
  input contexts, and unbound-key support.
- Shared UI kit under the `--osf-*` CSS namespace, plus TypeScript
  definitions (`sdk/osfui.d.ts`).
- Developer loop: `devMode` verbose logging and first-frame PNG dump,
  **F11** in-place view reload, schema hot-reload, and a browser dev harness.

### For plugin authors

- Native C++ API (`sdk/OSFUI_API.h`): register shipped views at runtime and
  handle commands from your views.

### Engine integration

- Views open as real engine menus: proper menu-stack admission, game pause
  while the overlay is open, and a hardware cursor.
- Shipped `OSFUI/config.json` is the developer/boot file; user-facing
  settings live in the in-game menu and survive updates.
- The default build has zero Ultralight footprint; the real renderer is an
  opt-in build (`xmake f --with_ultralight=true`).
