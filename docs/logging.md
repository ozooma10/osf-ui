# Logging convention

The logs exist to answer one question after a bad session: **what was OSF UI doing
when things went wrong?** Every line either helps answer that or costs the reader
time. This doc is the contract; `src/Core/Log.h` points here.

Component and health names follow the [terminology glossary](terminology.md).

## Files

| File | Writer | Kept |
|---|---|---|
| `SFSE\Logs\OSF UI.log` | plugin (spdlog via REX) | current + previous session (`OSF UI.1.log`, spdlog rotate-on-open) |
| `SFSE\Logs\OSF UI.webview2-host.log` | browser-host executable (custom `Logger`) | current + previous session (`*.old.log`, renamed on open) |

Retention matters because a crash log is only useful until the next launch —
both files used to truncate on open, destroying the evidence exactly when the
user relaunches to "try again" before reporting.

Both patterns carry `[MM-DD HH:MM:SS.mmm]` so the two files (and multi-day
installs) correlate at a glance. The plugin log also carries the thread id.

There is a third destination that is not a file: **the view's own console**,
which is where everything a mod author can get wrong goes. See *Author-caused
failures go to the page* below — it is a peer of these two, not an afterthought.

## Levels

The floor is **Info** in normal play, **Debug** in developer mode (enabled by
`devMode` or the temporary author-mode marker and set once in
`Log::SetDevMode`, called right after config load; boot runs at Debug so nothing
pre-config is lost). `flush_on` tracks the floor, so everything kept survives a
crash. Consequence: **a line's level decides whether it exists in a user's bug
report.** Choose accordingly:

- **ERROR** — the OSF UI runtime, browser host, or web renderer is broken or
  degraded: hooks failed self-test, the browser host died, file writes fail.
  A user seeing this should expect visible breakage.
- **WARN** — something unexpected that OSF UI survived, worth a human's
  attention. Not the default for input validation — see `[content]` below.
- **INFO** — one-shot (or near-one-shot) state transitions that reconstruct the
  session timeline: boot milestones, config summary, hooks armed, views
  loaded/ready, features enabled, teardown. This is the crash-forensics band:
  "how far did it get, what was live." If it can repeat every frame/event, it
  does not belong here.
- **DEBUG** — per-event / per-call chatter and research-probe samples. Only
  visible in developer mode. High-frequency sites should *also* be bounded
  (sampling, budgets, once-flags) so developer-mode logs stay readable.
- TRACE and CRITICAL are unused; don't start.

## Prefixes and tags

- Every line starts with its module: `Runtime:`, `SettingsStore:`, `BridgeApi:`, …
  Composite code may use an established bracketed tag such as `[UiPassSeam]`.
  Pick one existing style; don't invent hybrids.
- **`[content]`** — placed after the module prefix — marks *third-party content
  errors*: a mod's manifest/schema/overlay/API call is wrong, OSF UI itself is
  fine. Same level as before (mod authors and "why is my mod's UI missing"
  reports need them visible), but `grep -v "\[content\]"` strips them instantly
  when hunting an OSF UI fault. Conversely, `grep "\[content\]"` is the mod
  author's view.
- Browser-host lines relayed into the plugin log keep the `WebView2 host:` prefix;
  expect them duplicated in both files (by design — either file alone should
  tell the story).
- Forwarded **page console** output is the one third-party category that does
  *not* carry `[content]`: it arrives as `Runtime: view '<id>' console: …`, and
  the `console:` infix is what you grep for. Worth knowing before you conclude
  from a lone ERROR line that the plugin broke — the text after `console:` was
  written by a view, and a view's `console.error` is an author's bug, not OSF
  UI's.

## Repetition budget

Nothing may log unboundedly. Established tools, in preference order:

1. State-change-only: log on transition, not on state (most INFO lines).
2. One-shot flags / `Log::WarnOnce` (`std::once_flag`, once per process).
3. Resettable once-per-episode flags where re-occurrence is meaningful
   (`PauseMenuEntry`'s local `WarnOnce`: once per pause-menu open — deliberate,
   not a duplicate to unify).
4. Dedupe-by-key with a "further rejections not logged" note
   (`MessageBridge`'s unknown-endpoint warning, keyed by endpoint name and
   capped at 512 distinct names, so a page polling a dead endpoint costs one
   line rather than one per attempt).
5. Bounded sampling: dense first-N then logarithmic for high-frequency diagnostics.
6. Fold a matched exchange into one line rather than tracing both legs
   (`MessageBridge::NoteTracedReply`: everything sent back while one inbound
   message is being dispatched — the settlement, plus any event or state the
   handler triggered — is collapsed into that message's single completion line,
   so a healthy request costs one line and an unanswered one still reads
   `(nothing)`).

There is one deliberate hole: **reported protocol faults log every
occurrence.** `MessageBridge::ReportProtocolFault` writes a `[content]` WARN each time a
view's message is refused, and only the accompanying explanatory line is
deduped by endpoint name. A page in a retry loop will therefore repeat that
warning (bounded only by the browser host's 128 messages/second per-view rate cap). It
is loud on purpose: the alternative is silence about a mod author's mistake,
which is the exact failure 2.0's error routing exists to remove. If a real
report ever drowns in these, the fix is a per-view+code throttle, not a global
once-flag that would hide the second mod's problem behind the first.

A probe that has answered its question is deleted, not left logging. Research
instrumentation earns its keep only while the question is open; once the finding
is written down, the probe is redundant, and a stale one drifts into being wrong
(`ThreadAffinityProbe` printed every stack frame as `Starfield.exe+<offset>`
without checking the frame was in that module, so DLL frames showed as ~14 GB
offsets — deleted 2026-07-29).

## Author-caused failures go to the page

OSF UI does not build its own inspector. The debugger mod authors already have
— Chromium DevTools opens with F12 on the active menu in developer mode — is the
debugging destination, and the rule that makes it sufficient is: **every failure a mod author
can cause is printed to that view's own console**, not only to a log file the
author is not watching.

Two producers, one sink, one prefix (`[osfui]`, from
`frontend/src/shared-kit/osfui.js`):

- **Client-detected**, printed by the shared bridge helper as `console.error`: every request
  rejection (`request "<name>" failed: <code> — <message>` plus the rejecting
  payload as an inspectable object), `no-bridge`, client timeouts, and a
  handler of yours that threw inside an event or state callback. A plain-browser
  preview is not a mistake, so *that* one is a `console.warn` notice.
- **OSF UI runtime-detected protocol faults**, delivered to the affected view
  as the developer-mode-only `osfui.debug.error` event and printed the same way
  (`OSF UI runtime rejected <code>: <message>`): a send dropped for naming a
  request endpoint, an unknown endpoint, or a malformed envelope. These are
  view-caused faults a page would otherwise never hear about because a `send`
  has no reply channel.
- **Endpoint-handler timeouts**, rejected to the waiting page as
  `no-response` and printed as an ordinary request rejection: a mod backend or
  OSF UI runtime handler missed the OSF UI runtime's 30 s deadline. This is not
  a view-caused fault and never counts against that view's protocol-fault
  budget.

Set `localStorage["osfui:trace"] = "1"` and reload the view to add a
`console.debug` per envelope in both directions (`[osfui] ->` / `[osfui] <-`,
the envelope object, and settlement latency in ms on replies). The flag is read
once at shared bridge helper load, is per view, and costs nothing when off. DevTools' own
filtering, object inspection and preserve-log then do everything a bespoke
traffic inspector would.

Because developer mode also forwards page console output over the pipe, all of the
above lands in `OSF UI.log` too, with no second channel to maintain:

| Page call | Plugin log level | Line |
|---|---|---|
| `console.error` (and uncaught exceptions, pre-shaped as `uncaught: …`) | ERROR | `Runtime: view '<id>' console: …` |
| `console.warn` | WARN | same |
| `console.log` / `.info` / `.debug` | DEBUG | same |

Two consequences worth planning for. Page chatter deliberately sits at DEBUG —
it is not a diagnosis signal — so a developer-mode session with the trace flag on turns
the plugin log into a full bridge capture; that is useful and expensive, and it
is why the flag is opt-in per view rather than a config key. And console
forwarding is registered only in developer mode (a release build would cross the pipe
just to be dropped), so a player's bug report contains none of this: for a
player-visible condition, raise a health issue; don't `console.error` and hope.

The native side of the same events is logged whether or not anyone is watching
the page. The lines that matter when reading a bad session:

| Line | Level | Means |
|---|---|---|
| `MessageBridge: view '<id>' greeted — ready, state replay, events open` | DEBUG | that document's boot completed |
| `MessageBridge: '<name>' from view '<view>' -> <what>` | DEBUG | one inbound message and everything it produced (`reply`, `error:<code>`, `deferred`, `ready+state`, `(nothing)`) |
| `MessageBridge: [content] view '<id>': <code> — <message>` | WARN | refused message (the page got the same text) |
| `MessageBridge: [content] dropped send to unknown endpoint '<name>'` | WARN | once per name |
| `MessageBridge: '<name>' from view '<view>' missed the 30s OSF UI runtime deadline` | WARN | a mod backend or OSF UI runtime handler never settled a deferred request |
| `MessageBridge: request endpoint '<name>' returned without settling` | ERROR | platform bug — the caller got `internal` |
| `MessageBridge: [web] <text>` | DEBUG | the view's own `log` send, truncated at 512 chars |

Unsolicited outbound pushes are deliberately near-silent: state logs one DEBUG
line for the four platform keys (`settings`, `views`, `diagnostics`, `i18n`) and
nothing for a mod's own keys, and an unsolicited event logs nothing at all —
`ui.gamepad` and friends push far too often to trace. Turn on the page-side
trace flag when you need to see those; that is what it is for.

## Browser-host process specifics

The browser-host logger has three levels: `Info` (file-only), `Warn`/`Error`
(auto-forwarded to the plugin log), and `InfoFwd` (explicit milestone opt-in
that lands in both). Reserve `InfoFwd` for lines a plugin-log-only reader needs
for the session timeline; periodic diagnostics stay file-only and must remain
bounded and time-throttled.

## What crash forensics needs at INFO (checklist)

Boot: SFSE milestones, config summary, effective developer mode and activation source. Hooks:
every vtable/trampoline hook armed or refused (address + slot). Renderer: browser-host
launch, device/queue located, seam enabled. Views: loaded / finished loading /
recovered / terminally failed / destroyed. Teardown: shutdown reached. If a crash log's
last line leaves you unable to say which of these were live, promote the missing
transition.

One known blind spot in that band: the **greeting** is DEBUG, so a default-level
log shows `view '<id>' finished loading` but cannot distinguish "the document
never greeted the bridge" (nothing rendered, no state was ever replayed) from
"it greeted and rendered nothing". That is the first question a blank-view
report raises, and answering it currently costs a developer-mode repro. Promote it if
blank-view reports keep arriving.
