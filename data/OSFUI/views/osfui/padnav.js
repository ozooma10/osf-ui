// padnav.js — gamepad/keyboard focus navigation for the first-party OSF UI
// views (settings, keybinds).
//
// The runtime's default gamepad mapping (Runtime::DrainEngineInput) turns
// D-pad / left-stick edges into injected arrow-key taps, A into Enter and B
// into close — so a view has controller support exactly when its DOM is
// arrow-key navigable. These pages are mouse-first (click handlers on
// buttons/rows); this layer adds the missing focus model:
//
//   Arrows      move focus to the nearest interactive element in that
//               direction (spatial — no zone bookkeeping, so it works for the
//               rail list, settings rows, the Home card grid and the keyboard
//               board alike)
//   Enter (A)   activates the focused element (click) — buttons, switches,
//               rows; on a text input it commits (blur + refocus)
//   Left/Right  on a range slider adjust the value (WebCore's native
//               handling); on a <select> they cycle the options
//   B / Esc     stay native (the runtime closes the top menu itself)
//
// Keyboard-only users get the same fix for free: per the frozen input
// contract, gamepad support == arrow-key support.
//
// Deliberately PRIVATE to the osfui views (loaded as ../padnav.js) — not part
// of the shared kit: osfui.js is frozen (api-freeze item 5) and gamepad is
// still experimental, so this must be able to change shape freely.
//
// Coupling notes (fine for a private helper):
//   - `[data-nav-modal]` on an overlay scopes navigation inside it (the
//     settings undo panel sets it).
//   - While a key-rebind capture is armed (an element with class "listening"
//     exists) all navigation is suspended — the next key press belongs to the
//     capture, not to us.
//   - `.row:focus-within` CSS in the views makes hover-only affordances (the
//     per-setting reset button) appear when focus lands on them.

"use strict";

(function () {
  const html = document.documentElement;

  // A universal focus ring, active only while navigating by key. The kit's
  // own :focus-visible rules override this for kit controls (they carry their
  // own accent styling); this catches everything else (rail items, list rows,
  // board keys, home cards). Removed on mousedown so mouse users never see
  // rings appear from an earlier controller session.
  const style = document.createElement("style");
  style.textContent =
    "html.padnav-kb :focus { outline: 2px solid var(--osf-accent, #5aa9b8); outline-offset: 1px; }";
  document.head.appendChild(style);
  document.addEventListener("mousedown", () => html.classList.remove("padnav-kb"), true);

  // Where focus last was — survives the views' teardown-and-rebuild renders
  // (selecting a rail item rebuilds both panes), so navigation resumes from
  // the same place instead of restarting at the top-left.
  let lastRect = null;

  // Ultralight builds key events from VK codes and legacy key identifiers, so
  // `e.key` can be the legacy "Up"/"Down" spelling (or a "U+00XX" string) —
  // keyCode is the invariant across Ultralight and plain browsers.
  function keyNameOf(e) {
    switch (e.keyCode) {
      case 13: return "enter";
      case 37: return "left";
      case 38: return "up";
      case 39: return "right";
      case 40: return "down";
      default: return "";
    }
  }

  function navRoot() {
    return document.querySelector("[data-nav-modal]") || document;
  }

  // Every visible, enabled interactive element in the current scope. A
  // display:none ancestor (collapsed group, hidden-cond row, [hidden]) yields
  // a zero rect, which is the visibility test.
  function candidates() {
    const list = [];
    for (const el of navRoot().querySelectorAll(
      "button, input, select, textarea, a[href], [tabindex]")) {
      if (el.disabled || el.tabIndex < 0) continue;
      const r = el.getBoundingClientRect();
      if (r.width < 1 || r.height < 1) continue;
      list.push({ el, r });
    }
    return list;
  }

  const cx = (r) => r.left + r.width / 2;
  const cy = (r) => r.top + r.height / 2;

  // Nearest candidate strictly in `dir` from `from`, center-to-center, with
  // off-axis drift penalized so vertical travel stays in its column (rail vs
  // detail) and horizontal travel stays in its row.
  function pickDirectional(from, dir, cands) {
    const fx = cx(from), fy = cy(from);
    let best = null, bestScore = Infinity;
    for (const c of cands) {
      const dx = cx(c.r) - fx, dy = cy(c.r) - fy;
      let primary, secondary;
      if (dir === "up") { primary = -dy; secondary = Math.abs(dx); }
      else if (dir === "down") { primary = dy; secondary = Math.abs(dx); }
      else if (dir === "left") { primary = -dx; secondary = Math.abs(dy); }
      else { primary = dx; secondary = Math.abs(dy); }
      if (primary <= 1) continue;
      const score = primary + secondary * 2.5;
      if (score < bestScore) { bestScore = score; best = c.el; }
    }
    return best;
  }

  function nearest(rect, cands) {
    const fx = cx(rect), fy = cy(rect);
    let best = null, bestScore = Infinity;
    for (const c of cands) {
      const dx = cx(c.r) - fx, dy = cy(c.r) - fy;
      const score = dx * dx + dy * dy;
      if (score < bestScore) { bestScore = score; best = c.el; }
    }
    return best;
  }

  function focusEl(el) {
    el.focus();
    lastRect = el.getBoundingClientRect();
    // Options object is fine here: the views already rely on it elsewhere
    // (renderSearch's block:"center"), so this Ultralight supports it.
    if (el.scrollIntoView) el.scrollIntoView({ block: "nearest", inline: "nearest" });
  }

  // Anything focus lands on (mouse clicks included) becomes the resume point.
  document.addEventListener("focusin", (e) => {
    if (e.target && e.target.getBoundingClientRect) {
      lastRect = e.target.getBoundingClientRect();
    }
  });

  function isTextEntry(el) {
    if (el.tagName === "TEXTAREA") return true;
    if (el.tagName !== "INPUT") return false;
    return !["range", "checkbox", "radio", "button", "submit", "reset", "color"].includes(el.type);
  }

  // <select>: cycle options directly (deterministic — Ultralight has no
  // dropdown popup to fall back on) and fire the change the views commit on.
  function adjustSelect(el, delta) {
    const n = el.options.length;
    if (!n) return;
    const i = Math.min(n - 1, Math.max(0, el.selectedIndex + delta));
    if (i === el.selectedIndex) return;
    el.selectedIndex = i;
    el.dispatchEvent(new Event("change", { bubbles: true }));
  }

  // After an activation that rebuilds the pane (rail select, preset, reset),
  // the focused element is gone and focus falls to <body>; put it back on
  // whatever now sits where focus was.
  function scheduleRefocus() {
    setTimeout(() => {
      const a = document.activeElement;
      if (a && a !== document.body && a !== html) return;
      if (!lastRect) return;
      const next = nearest(lastRect, candidates());
      if (next) focusEl(next);
    }, 0);
  }

  document.addEventListener("keydown", (e) => {
    const name = keyNameOf(e);
    if (!name) return;
    if (e.ctrlKey || e.altKey || e.metaKey || e.shiftKey) return;
    // A rebind capture is armed: the next key belongs to it (in-game it is
    // consumed natively anyway; this covers the standalone preview path).
    if (document.querySelector(".listening")) return;

    let active = document.activeElement;
    if (active === document.body || active === html) active = null;
    const modal = document.querySelector("[data-nav-modal]");
    if (modal && active && !modal.contains(active)) active = null;

    if (active) {
      const tag = active.tagName;
      if (tag === "INPUT" && active.type === "range") {
        // Left/right adjust the slider — WebCore's own handling, which
        // fires the input/change events the views commit on. Up/down
        // fall through to navigation (preventDefault below stops the
        // native value change).
        if (name === "left" || name === "right") return;
      } else if (tag === "SELECT") {
        if (name === "left" || name === "right") {
          e.preventDefault();
          adjustSelect(active, name === "left" ? -1 : 1);
          return;
        }
        if (name === "enter") return;
        // up/down fall through to navigation.
      } else if (isTextEntry(active)) {
        if (name === "left" || name === "right") return;  // caret movement
        if (name === "enter") {
          if (tag === "TEXTAREA") return;  // newline
          e.preventDefault();
          active.blur();   // commit (the views listen on change)
          active.focus();  // keep the navigation position
          return;
        }
        if (tag === "TEXTAREA") {
          // Navigate only from the caret's boundary, so arrows still
          // move the caret through the text in between.
          const atStart = active.selectionStart === 0 && active.selectionEnd === 0;
          const atEnd = active.selectionStart === active.value.length &&
            active.selectionEnd === active.value.length;
          if ((name === "up" && !atStart) || (name === "down" && !atEnd)) return;
        }
      }
    }

    if (name === "enter") {
      if (active && active.tagName !== "SELECT" && !isTextEntry(active)) {
        e.preventDefault();
        lastRect = active.getBoundingClientRect();
        html.classList.add("padnav-kb");
        active.click();
        scheduleRefocus();
      }
      return;
    }

    // ---- arrow navigation ----
    e.preventDefault();
    html.classList.add("padnav-kb");
    const cands = candidates();
    if (!cands.length) return;
    let next;
    if (active) {
      next = pickDirectional(active.getBoundingClientRect(), name,
        cands.filter((c) => c.el !== active));
    } else if (lastRect) {
      // A re-render dropped focus: resume near where it last was.
      next = nearest(lastRect, cands);
    } else {
      // Fresh page, nothing focused yet: start on the first real control,
      // not the search box (a gamepad can't type into it anyway).
      const first = cands.find((c) => !isTextEntry(c.el)) || cands[0];
      next = first.el;
    }
    if (next) focusEl(next);
  });

  window.padnav = {
    // Forget the resume point (a fresh overlay visit starts navigation over).
    reset() { lastRect = null; },
    // Focus the first visible match of `selector` (or the first candidate).
    focusFirst(selector) {
      const cands = candidates();
      if (!cands.length) return;
      if (selector) {
        const hit = cands.find((c) => c.el.matches(selector));
        if (hit) { focusEl(hit.el); return; }
      }
      focusEl(cands[0].el);
    },
  };
})();
