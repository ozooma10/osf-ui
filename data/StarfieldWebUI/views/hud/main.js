// Demo HUD: a SECOND interactive view, composited over the active one. It shows
// per-view bridge routing end to end:
//   - native -> THIS view: receives its OWN runtime.ready (status line) and its
//     OWN runtime.pong (so a ping from here comes back HERE, not to settings);
//   - THIS view -> native: posts a log on load (attributed to "hud") and a ping
//     when its button is clicked.
// Focus it in-game with the focus key (Tab) and click "Ping native". Runs fine
// in a plain browser too (degraded: no bridge).
"use strict";

const clockEl = document.getElementById("clock");
const statusEl = document.getElementById("status");
const pongEl = document.getElementById("pong");
const cursorEl = document.getElementById("cursor");

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

function send(command, fields = {}) {
  if (bridgeAvailable()) {
    window.starfield.postMessage(JSON.stringify({ type: "ui.command", payload: { command, ...fields } }));
  }
}

// native -> THIS view
window.starfield = window.starfield || {};
window.starfield.onMessage = (json) => {
  let msg;
  try { msg = JSON.parse(json); } catch { return; }
  switch (msg.type) {
    case "runtime.ready":
      statusEl.textContent = `bridge: online · ${msg.payload.plugin} v${msg.payload.version}`;
      break;
    case "runtime.pong":
      pongEl.textContent = `pong ✓ @ ${new Date().toLocaleTimeString()}`;
      break;
    default:
      break;
  }
};

// Software pointer follows routed mouse moves (only delivered while focused).
document.addEventListener("mousemove", (e) => {
  if (cursorEl) {
    cursorEl.style.left = `${e.clientX}px`;
    cursorEl.style.top = `${e.clientY}px`;
  }
});

document.getElementById("ping").addEventListener("click", () => {
  pongEl.textContent = "ping…";
  send("ping");
});

// One-shot log on load, attributed to "hud" natively.
if (bridgeAvailable()) {
  send("log", { text: "hud layer online" });
} else {
  statusEl.textContent = "bridge: standalone (browser)";
}
