// Passive HUD demo: a second view, composited over the active one by the
// renderer. It uses NO native bridge — it only proves that an independent,
// live, repainting view is blended on top of the menu. Each clock tick (and
// the CSS pulse) repaints this view, which the renderer re-composites. Runs
// identically in a plain browser.
"use strict";

const clockEl = document.getElementById("clock");

function tick() {
  const d = new Date();
  const pad = (n) => String(n).padStart(2, "0");
  clockEl.textContent = `${pad(d.getHours())}:${pad(d.getMinutes())}:${pad(d.getSeconds())}`;
}

tick();
setInterval(tick, 1000);
