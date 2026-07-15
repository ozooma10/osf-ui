# In-game verification: ControlLayer + FocusMenu (the gamepad-leak fix)

**Status: RESOLVED — verified in-game on 1.16.244 (2026-07-02, re-exercised
2026-07-15).** `focusMenu`, `disableControls`, `engineInput`, and
`pauseMenuEntry` are on by default in code (`src/core/Config.h`) and no longer
carry EXPERIMENTAL labels. Confirmed live: ControlLayer freezes keyboard +
mouse-look + gamepad and restores cleanly (the gamepad leak is FIXED); the
hardened FocusMenu is admitted to the menu stack (Route A), survives long
sessions, and tears down without the post-close RTTI crash (kHide delegated to
the engine base); FreeCursor releases the OS pointer with no frozen center
arrow; SimPause pauses/resumes and stays balanced under mashing; EngineInput
delivers gamepad to the runtime and routes it into the view; the PauseMenu
"MOD SETTINGS" entry injects and opens the overlay.

**Residual, still un-signed-off (track as known limitations, not blockers):**
quickload *while paused* (the one untested crash-hazard path); gamepad-B vs
engine menu-back desync under heavy use (§C); HDR/SDR-degrade smoke on real HDR
hardware; and overlay coexistence with ReShade / RTSS / Steam overlay /
frame-gen. The checklist below is retained as the regression procedure.

## What is already proven vs. what this run proves

Proven by the OSF RE probes (2026-06-13, on 1.16.244 — mechanism level):

- `BSInputEnableManager`/`BSInputEnableLayer` control-disable freezes
  keyboard + mouse-look + **gamepad sticks**, and restores cleanly
  (probe ran live with a controller).
- Custom `IMenu` registration + open works; the headless-menu crash was
  root-caused (null `menuName` @ +0xB0) and the hardened creator fixes it
  (engine base-init + engine vtable copy + interned name).

NOT yet proven (this run's job):

1. OSF UI's **in-tree integration** of both (Runtime reconcilers, the
   open/close lifecycle) behaves in real gameplay.
2. The hardened FocusMenu **survives past the few-second mark** where the
   headless one crashed, across long sessions and repeated open/close.
3. No engine path (gamepad B / menu-back) desyncs or force-closes the menu
   behind the runtime's back.

## Preconditions

- [ ] Fresh deploy is live. The build logs
      `Plugin: focusMenu on — registering OSFUI_FocusMenu` — if the log still
      says `(EXPERIMENTAL) … long-session survival is pending` or "custom-IMenu
      registration is unproven" instead, the game is running an OLD dll (VFS
      race / wrong mod enabled — check `MO2\mods\OSF UI` is the enabled mod).
- [ ] XInput controller connected and working in gameplay before the test.
- [ ] Config is the shipped one: `focusMenu: true`, `disableControls: true`,
      `captureInput: true`, `inputSource: "ui"`.
- [ ] Log open: `Documents\My Games\Starfield\SFSE\Logs\OSF UI.log`.

## A. ControlLayer — gamepad freeze (the headline bug)

In gameplay (save loaded, on foot, controller in hand):

- [ ] Open the overlay (F10). Log shows
      `ControlLayer: player controls disabled (layer N)`.
- [ ] Left stick — player must NOT move.
- [ ] Right stick — camera must NOT turn.
- [ ] A / X / Y / B, triggers, bumpers — no activate, no fire, no jump, no
      POV change. (B is also test C — note what it does.)
- [ ] Keyboard WASD + mouse-look — also frozen (double-covered by the
      WndProc; regression check).
- [ ] Close (F10). Log shows `ControlLayer: player controls restored`;
      movement/camera/fire all work again — on BOTH controller and KB/M.
- [ ] Repeat open/close ~10×; controls always freeze/restore, log pairs up.

## B. FocusMenu — survival + cursor

- [ ] First open after launch: log shows
      `FocusMenu: creator built engine-initialised menu obj=0x… (uiMovie=null,
      name@+0xB0 set)`.
- [ ] Leave the overlay OPEN for 5+ minutes while interacting with the page
      (click, type in a text field, scroll). The headless menu died within
      seconds — this is the explicit survival test. No crash, no freeze.
- [ ] Cursor check: is the ENGINE cursor visible on top of the page's own
      CSS pointer (double cursor)? Note what you see — expected possible
      outcomes: engine arrow + CSS pointer both visible (cosmetic issue to
      fix), or only one.
- [ ] Close and re-open several times across a session; play normally
      (>15 min) with occasional toggles. No crash at any point.

## C. Gamepad B / engine menu-back while open

`ControlLayer` deliberately leaves the `Menu` user-event bits ENABLED, so the
engine's own menu navigation may still see the controller. The risk: the
engine closes `OSFUI_FocusMenu` itself (menu-back), desyncing it from the
overlay (which stays visible and capturing).

- [ ] With the overlay open, press B (and Start, and D-pad) on the controller.
- [ ] Watch the log for an unexpected `OSFUI_FocusMenu` close event
      (MenuEventSink logs menu open/close) without a matching
      `FocusMenu: close requested`.
- [ ] Symptoms of desync: overlay still drawn but engine cursor vanishes /
      pause menu opens underneath / after F10-closing, input state is wrong.
- [ ] If B does close it: that's a finding, not a failure — note it; the fix
      is on me (track engine-driven closes via MenuEventSink and reconcile).

## D. Edge cases

- [ ] **Main menu:** toggle the overlay before loading a save. Expected: it
      opens; log shows the warn-once
      `ControlLayer: BSInputEnableManager not ready (main menu?)`; no crash.
      After loading into gameplay with the overlay still open, controls must
      be frozen (the reconciler retries every tick).
- [ ] **Save → load with overlay open:** does the overlay auto-hide on the
      loading screen? After the load, are controls alive (no stuck
      input-enable layer)? Save while the overlay is open, reload that save,
      confirm controls.
- [ ] **Game's own pause menu:** Esc-close the overlay first, open the pause
      menu, close it — nothing odd. Then: overlay open + engine pause menu
      interplay if reachable at all.
- [ ] **Travel verbs:** with the overlay open, try fast travel / grav jump
      binds — should be inert (`FastTravel`/`GravJump` flags are in the
      disable set).

## E. (Optional, same session) URL crash-recovery smoke

Landed 2026-07-01, also unverified. Cheap to check while you're in:

- [ ] Before launch, temporarily rename a loaded view's entry file (e.g.
      `views/hud/index.html` → `index.html.bak`). In-game the log should show
      `FAILED to load` → `reload attempt 1/3 scheduled in 2s` → attempts
      counting up.
- [ ] Restore the file *before* the 3rd retry fires → log shows
      `crash-recovery reloading view` then `view '…' recovered`, and the view
      appears.
- [ ] (Or let all 3 fail → `giving up — destroying the view`, and the
      overlay/menu for that view can no longer be opened; F10 on a destroyed
      default view logs a WARN instead of showing an empty capture overlay.)

## F. Report back

Send the full `OSF UI.log` from the session plus, per section, pass/fail and
anything odd (double cursor, B-button behavior, any input that leaked).

## Decision table

| Outcome | Action |
|---|---|
| A+B pass | Flip both defaults to `true` in `Config.h`, drop EXPERIMENTAL from README/ROADMAP/troubleshooting; gamepad leak documented as FIXED. |
| A passes, B fails (FocusMenu crash) | Ship `disableControls` on-by-default alone — it is the gamepad fix; `focusMenu` stays experimental/off with the crash noted in RE notes. |
| A fails (gamepad leaks) | Capture exactly which inputs leak; back to RE (flag set is provisional per README). Loud known-limitation in README + startup log warning meanwhile. |
| C shows engine-driven closes | Keep defaults pending; I wire MenuEventSink reconciliation for our own menu, then re-test C only. |
