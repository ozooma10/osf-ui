// Demo HUD: a SECOND interactive view, composited over the active one. It shows:
//   - per-view bridge routing (its OWN runtime.ready + runtime.pong),
//   - a Ping button (focus with Tab, then click — pong comes back HERE),
//   - BINDING TO REAL GAME DATA: it polls game.get and displays the in-game
//     date/time, which native reads from RE::Calendar on the game thread.
// Degraded in a plain browser (no bridge): shows a real-world clock instead.
"use strict";

const clockEl = document.getElementById("clock");
const statusEl = document.getElementById("status");
const pongEl = document.getElementById("pong");
const cursorEl = document.getElementById("cursor");

function bridgeAvailable() {
  return typeof window.prisma === "object" &&
         typeof window.prisma.postMessage === "function";
}
function send(command, fields = {}) {
  if (bridgeAvailable()) {
    window.prisma.postMessage(JSON.stringify({ type: "ui.command", payload: { command, ...fields } }));
  }
}
function pad2(n) { return String(n).padStart(2, "0"); }
function fmtHour(h) {
  const hh = Math.floor(h);
  const mm = Math.floor((h - hh) * 60);
  return `${pad2(hh)}:${pad2(mm)}`;
}

// native -> THIS view
window.prisma = window.prisma || {};
window.prisma.onMessage = (json) => {
  let msg;
  try { msg = JSON.parse(json); } catch { return; }
  switch (msg.type) {
    case "runtime.ready":
      statusEl.textContent = `bridge: online · ${msg.payload.plugin} v${msg.payload.version}`;
      break;
    case "runtime.pong":
      pongEl.textContent = `pong ✓ @ ${new Date().toLocaleTimeString()}`;
      break;
    case "game.data": {
      const p = msg.payload;
      clockEl.textContent = p.available
        ? `in-game · Y${p.year} M${pad2(p.month)} D${pad2(p.day)} · ${fmtHour(p.hour)}`
        : "in-game · (no save loaded)";
      break;
    }
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

// Poll the in-game clock once a second; degrade to a real-world clock offline.
function poll() {
  if (bridgeAvailable()) {
    send("game.get");
  } else {
    const d = new Date();
    clockEl.textContent = `local · ${pad2(d.getHours())}:${pad2(d.getMinutes())}:${pad2(d.getSeconds())}`;
  }
}
poll();
setInterval(poll, 1000);

// One-shot log on load, attributed to "hud" natively.
if (bridgeAvailable()) {
  send("log", { text: "hud layer online" });
} else {
  statusEl.textContent = "bridge: standalone (browser)";
}
