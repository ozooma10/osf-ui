# Changelog

Notable changes to OSF UI. Versions are `kPluginVersion` (`src/core/Version.h`);
the native bridge ABI (`sdk/OSFUI_API.h`) and the web bridge protocol version
independently and are called out per entry.

## 0.2.0 — 2026-07-13

### Native bridge ABI 1.3 — SendToWeb delivery guarantee (no vtable change)

`SendToWeb` to a loaded, bridge-enabled view is now **queued until the view can
receive it** instead of being dropped or arriving late, and queued messages are
delivered **before the view's first visible paint** after a
`RequestMenu(viewId, true)`. Consumers can open a view directly in a target
state — `SendToWeb(v, mode); RequestMenu(v, true);` — with no flash of the
page's default face, and can retire workarounds like OSF Animation's
"veil" (hide-until-first-mode-push). Detect via
`(GetInterfaceVersion() & 0xFFFF) >= 3`.

- `SendToWeb` before the bridge is live is queued (previously returned `false`
  / dropped) and flushed FIFO once a bridge appears; it now returns `false`
  only on null arguments or unparseable payload JSON.
- Messages to a page that is still loading, or that has not yet installed
  `osfui.onMessage`, are held per view — and the view is kept **off screen**
  until every held message is delivered, so a visible paint can never precede
  a queued message.
- `Runtime::Tick` now flushes queued sends **before** applying queued
  `RequestMenu` opens, and the renderer worker snapshots each view's hidden
  flag together with its message queue, closing the cross-thread race where an
  unhide could out-run a message queued before it.
- On the closed→open edge the compositor reveal is deferred until the renderer
  hands over a frame produced after the open, so the present hook can no
  longer re-draw stale pre-open overlay content (this also removes the
  stale-frame flash on reopen that existed independently of messaging).
- All native→web queues are bounded per view and drop the **oldest** message
  on overflow with a log warning (previously the newest was dropped, or the
  queue could grow unbounded pre-DOM-ready), so pushes to a never-opened view
  cannot grow memory and a spammed view still converges on the newest state.
- Sends to an unknown view id, or to a view whose manifest lacks
  `permissions.nativeBridge`, are now warned in the log (once per id) instead
  of silently dropped.
- Plugin version 0.1.0 → 0.2.0; `kBridgeAPIVersion` 1.2 → 1.3. The web bridge
  protocol (`bridgeVersion` 0.3) is unchanged.

Behavioral note for view authors: a bridge-enabled view that receives messages
stays hidden until it installs `osfui.onMessage` — install it during initial
script execution (all first-party views already do). Message order on an open
is now consumer-send order followed by `ui.visibility {visible:true}`;
previously `ui.visibility` could arrive first.

## 0.1.0

Baseline: Ultralight offscreen renderer + D3D12 present-hook compositor, menu/
HUD surface policy (focus, capture, sim pause, control layer), settings module
(MCM) with sparse write-behind persistence, native plugin API through ABI 1.2
(commands, sends, ready, `RequestMenu`, typed setting getters,
`SubscribeSettings`, `RegisterSettingsSchema`).
