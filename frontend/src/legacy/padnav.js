
"use strict";

(function () {
  const html = document.documentElement;

  const style = document.createElement("style");
  style.textContent =
    "html.padnav-kb :focus { outline: 2px solid var(--osf-accent, #5aa9b8); outline-offset: 1px; }";
  document.head.appendChild(style);
  document.addEventListener("mousedown", () => html.classList.remove("padnav-kb"), true);

  let lastRect = null;

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

  function bandOf(el, r) {
    const row = el.closest && el.closest(".row");
    return row ? row.getBoundingClientRect() : r;
  }

  const cx = (r) => r.left + r.width / 2;
  const cy = (r) => r.top + r.height / 2;

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

  function adjustSelect(el, delta) {
    const n = el.options.length;
    if (!n) return;
    const i = Math.min(n - 1, Math.max(0, el.selectedIndex + delta));
    if (i === el.selectedIndex) return;
    el.selectedIndex = i;
    el.dispatchEvent(new Event("change", { bubbles: true }));
  }

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
    if (document.querySelector(".listening")) return;

    let active = document.activeElement;
    if (active === document.body || active === html) active = null;
    const modal = document.querySelector("[data-nav-modal]");
    if (modal && active && !modal.contains(active)) active = null;

    if (active) {
      const tag = active.tagName;
      if (tag === "INPUT" && active.type === "range") {
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
