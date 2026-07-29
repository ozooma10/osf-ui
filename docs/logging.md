# Logging convention

The logs exist to answer one question after a bad session: **what was OSF UI doing
when things went wrong?** Every line either helps answer that or costs the reader
time. This doc is the contract; `src/core/Log.h` points here.

## Files

| File | Writer | Kept |
|---|---|---|
| `SFSE\Logs\OSF UI.log` | plugin (spdlog via REX) | current + previous session (`OSF UI.1.log`, spdlog rotate-on-open) |
| `SFSE\Logs\OSF UI.webview2-host.log` | host exe (custom `Logger`) | current + previous session (`*.old.log`, renamed on open) |

Retention matters because a crash log is only useful until the next launch —
both files used to truncate on open, destroying the evidence exactly when the
user relaunches to "try again" before reporting.

Both patterns carry `[MM-DD HH:MM:SS.mmm]` so the two files (and multi-day
installs) correlate at a glance. The plugin log also carries the thread id.

## Levels

The floor is **Info** in normal play, **Debug** with `devMode` (set once in
`Log::SetDevMode`, called right after config load; boot runs at Debug so nothing
pre-config is lost). `flush_on` tracks the floor, so everything kept survives a
crash. Consequence: **a line's level decides whether it exists in a user's bug
report.** Choose accordingly:

- **ERROR** — OSF UI (or its host/renderer) is broken or degraded: hooks failed
  self-test, host died, file writes failing. A user seeing this should expect
  visible breakage.
- **WARN** — something unexpected that OSF UI survived, worth a human's
  attention. Not the default for input validation — see `[content]` below.
- **INFO** — one-shot (or near-one-shot) state transitions that reconstruct the
  session timeline: boot milestones, config summary, hooks armed, views
  loaded/ready, features enabled, teardown. This is the crash-forensics band:
  "how far did it get, what was live." If it can repeat every frame/event, it
  does not belong here.
- **DEBUG** — per-event / per-call chatter and research-probe samples. Only
  visible in devMode. High-frequency sites should *also* be bounded (sampling,
  budgets, once-flags) so devMode logs stay readable.
- TRACE and CRITICAL are unused; don't start.

## Prefixes and tags

- Every line starts with its module: `Runtime:`, `SettingsStore:`, `BridgeApi:`, …
  Probe/composite code uses `[BracketedTag]` (`[UiPassSeam]`, `[WorldSurface]`).
  Pick one existing style; don't invent hybrids.
- **`[content]`** — placed after the module prefix — marks *third-party content
  errors*: a mod's manifest/schema/overlay/API call is wrong, OSF UI itself is
  fine. Same level as before (mod authors and "why is my mod's UI missing"
  reports need them visible), but `grep -v "\[content\]"` strips them instantly
  when hunting an OSF UI fault. Conversely, `grep "\[content\]"` is the mod
  author's view.
- Host lines relayed into the plugin log keep the `WebView2 host:` prefix;
  expect them duplicated in both files (by design — either file alone should
  tell the story).

## Repetition budget

Nothing may log unboundedly. Established tools, in preference order:

1. State-change-only: log on transition, not on state (most INFO lines).
2. One-shot flags / `Log::WarnOnce` (`std::once_flag`, once per process).
3. Resettable once-per-episode flags where re-occurrence is meaningful
   (`PauseMenuEntry`'s local `WarnOnce`: once per pause-menu open — deliberate,
   not a duplicate to unify).
4. Dedupe-by-key with a "further rejections not logged" note (`MessageBridge`).
5. Bounded sampling: dense first-N then logarithmic (`ScaleformToTextureProbe`,
   `WorldSurface` refresh), or fixed budgets (`ThreadAffinityProbe`).
6. Time-throttled periodics (render diagnostics, 2 s) — only behind an explicit
   opt-in setting (`renderStats`).

## Host process specifics

The host logger has three levels: `Info` (file-only), `Warn`/`Error`
(auto-forwarded to the plugin log), and `InfoFwd` (explicit milestone opt-in
that lands in both). Reserve `InfoFwd` for lines a plugin-log-only reader needs
for the session timeline; everything periodic stays file-only or behind
`renderStats`.

## What crash forensics needs at INFO (checklist)

Boot: SFSE milestones, config summary, devMode/author-mode state. Hooks:
every vtable/trampoline hook armed or refused (address + slot). Renderer: host
launch, device/queue located, seam enabled. Views: loaded / ready / recovered /
destroyed. Teardown: shutdown reached. If a crash log's last line leaves you
unable to say which of these were live, promote the missing transition.
