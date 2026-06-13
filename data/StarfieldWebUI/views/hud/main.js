// Passive HUD demo: a second view, composited over the active one by the
// renderer. It takes NO input (manifest interactive:false) but IS bridge-enabled
// (nativeBridge:true), so it demonstrates PER-VIEW bridge routing:
//   - native -> THIS view: it receives its OWN runtime.ready (not the active
//     view's), shown in the status line below;
//   - THIS view -> native: it posts a one-shot log that the native side
//     attributes to "hud" (see StarfieldWebUI.log).
// Runs fine in a plain browser too (degraded: no bridge).
"use strict";

const clockEl = document.getElementById("clock");
const statusEl = document.getElementById("status");

function tick() {
  const d = new Date();
  const pad = (n) => String(n).padStart(2, "0");
  clockEl.textContent = `${pad(d.getHours())}:${pad(d.getMinutes())}:${pad(d.getSeconds())}`;
}
tick();
setInterval(tick, 1000);

function bridgeAvailable() {
  return typeof window.starfield === "object" &&
         typeof window.starfield.postMessage === "function";
}

// native -> THIS view
window.starfield = window.starfield || {};
window.starfield.onMessage = (json) => {
  let msg;
  try { msg = JSON.parse(json); } catch { return; }
  if (msg.type === "runtime.ready") {
    statusEl.textContent = `bridge: online · ${msg.payload.plugin} v${msg.payload.version}`;
  }
};

// this view -> native: a one-shot log, attributed to "hud" natively.
if (bridgeAvailable()) {
  window.starfield.postMessage(JSON.stringify({
    type: "ui.command",
    payload: { command: "log", text: "hud layer online" },
  }));
} else {
  statusEl.textContent = "bridge: standalone (browser)";
}
