// padnav.js — gamepad/keyboard focus navigation for the first-party OSF UI
// views (settings, keybinds).
//
// The runtime's default gamepad mapping (Runtime::DrainEngineInput) turns
// D-pad / left-stick edges into injected arrow-key taps, A into Enter and B
// into close, so a view has controller support exactly when its DOM is
// arrow-key navigable — and keyboard-only users get the same fix. These pages
// are mouse-first, so this layer supplies the missing focus model: spatial
// arrow navigation, Enter to activate, B/Esc left native for the runtime to
// close the top menu.
//
// Private to the osfui views (loaded as ../padnav.js), not part of the shared
// kit: osfui.js is frozen (api-freeze item 5) and gamepad is still
// experimental, so this must be free to change shape.
//
// Coupling: `[data-nav-modal]` on an overlay scopes navigation inside it (the
// settings undo panel sets it), and `.row:focus-within` CSS in the views makes
// hover-only affordances (the per-setting reset button) appear when focus lands
// on them.

"use strict";

(function () {
  const html = document.documentElement;

  // A universal focus ring, active only while navigating by key. The kit's own
  // :focus-visible rules override it for kit controls; this catches everything
  // else (rail items, list rows, board keys, home cards). Removed on mousedown
  // so mouse users never see rings left over from a controller session.
  const style = document.createElement("style");
  style.textContent =
    "html.padnav-kb :focus { outline: 2px solid var(--osf-accent, #5aa9b8); outline-offset: 1px; }";
  document.head.appendChild(style);
  document.addEventListener("mousedown", () => html.classList.remove("padnav-kb"), true);

  // Where focus last was. Survives the views' teardown-and-rebuild renders
  // (selecting a rail item rebuilds both panes), so navigation resumes in place
  // instead of restarting at the top-left.
  let lastRect = null;

  // Use keyCode for consistent handling of browser, synthetic, and forwarded
  // Windows virtual-key events.
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
  // display:none ancestor (collapsed group, hidden-cond row, [hidden]) yields a
  // zero rect — that is the visibility test; own opacity 0 (the hover-only
  // per-setting reset chip) is invisible too and must not be a target.
  function candidates() {
    const list = [];
    for (const el of navRoot().querySelectorAll(
      "button, input, select, textarea, a[href], [tabindex]")) {
      if (el.disabled || el.tabIndex < 0) continue;
      const r = el.getBoundingClientRect();
      if (r.width < 1 || r.height < 1) continue;
      if (getComputedStyle(el).opacity === "0") continue;
      list.push({ el, r, band: bandOf(el, r) });
    }
    return list;
  }

  // Cross-axis distances are measured against the enclosing `.row` when there
  // is one (a settings row is one navigation line no matter where in it the
  // control sits), the element itself otherwise.
  function bandOf(el, r) {
    const row = el.closest && el.closest(".row");
    return row ? row.getBoundingClientRect() : r;
  }

  const cx = (r) => r.left + r.width / 2;
  const cy = (r) => r.top + r.height / 2;

  // Nearest candidate in `dir` from `from`. Direction is gated on the edge, not
  // the center: to count as "right" a candidate's center must clear the current
  // element's right edge (etc.), otherwise a same-column neighbour sitting a
  // few pixels off-center outranks a genuine sideways jump (the rail's undo
  // chip beating the whole detail pane).
  //
  // Off-axis distance is the gap between the two nav bands (0 when they
  // overlap), heavily penalized so travel stays in its column/row — measured on
  // `band`, not the element: a group header (left) and the next row's control
  // (right) share the pane's horizontal span, so Down enters the group instead
  // of skipping to the next header, while the rail and the detail pane
  // (disjoint spans) never bleed into each other. A small center-distance term
  // breaks ties inside one band in favour of the aligned element.
  function pickDirectional(from, fromBand, dir, cands) {
    const fx = cx(from), fy = cy(from);
    const gap = (a1, a2, b1, b2) => Math.max(0, Math.max(a1, b1) - Math.min(a2, b2));
    let best = null, bestScore = Infinity;
    for (const c of cands) {
      const x = cx(c.r), y = cy(c.r);
      let primary, offAxis, drift;
      if (dir === "up" || dir === "down") {
        if (dir === "up" ? y >= from.top : y <= from.bottom) continue;
        primary = Math.abs(y - fy);
        offAxis = gap(fromBand.left, fromBand.right, c.band.left, c.band.right);
        drift = Math.abs(x - fx);
      } else {
        if (dir === "left" ? x >= from.left : x <= from.right) continue;
        primary = Math.abs(x - fx);
        offAxis = gap(fromBand.top, fromBand.bottom, c.band.top, c.band.bottom);
        drift = Math.abs(y - fy);
      }
      const score = primary + offAxis * 2.5 + drift * 0.05;
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
    // Keep the element visible after spatial navigation moves focus.
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

  // <select>: cycle options directly for deterministic gamepad behavior and
  // fire the change event the views commit on.
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
        // Left/right adjust the slider via the browser's own handling, which
        // fires the input/change events the views commit on. Up/down fall
        // through to navigation (preventDefault below stops the native value
        // change).
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

    e.preventDefault();
    html.classList.add("padnav-kb");
    const cands = candidates();
    if (!cands.length) return;
    let next;
    if (active) {
      const fromR = active.getBoundingClientRect();
      next = pickDirectional(fromR, bandOf(active, fromR), name,
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
