// Test panel logic. Talks to the native runtime exclusively through the
// narrow JSON message bridge described in docs/security-model.md.
//
// The native side will eventually expose:
//   window.prisma.postMessage(jsonString)   // web -> native
//   window.prisma.onMessage(jsonString)     // native -> web (assigned here)
// Until a real renderer backend exists, this script detects the missing
// bridge and runs in a degraded "standalone" mode so the page is testable in
// an ordinary browser.

"use strict";

const statusEl = document.getElementById("status");
const logEl = document.getElementById("log");

function logLine(text) {
  const time = new Date().toLocaleTimeString();
  logEl.textContent += `[${time}] ${text}\n`;
  logEl.scrollTop = logEl.scrollHeight;
}

function bridgeAvailable() {
  return typeof window.prisma === "object" &&
         typeof window.prisma.postMessage === "function";
}

function sendToNative(type, payload) {
  const message = JSON.stringify({ type, payload: payload ?? {} });
  if (bridgeAvailable()) {
    window.prisma.postMessage(message);
    logLine(`-> native: ${message}`);
  } else {
    logLine(`(standalone, dropped) -> ${message}`);
  }
}

// Native -> web entry point. The native runtime calls this with a JSON string.
function onNativeMessage(jsonText) {
  let message;
  try {
    message = JSON.parse(jsonText);
  } catch {
    logLine(`<- native: unparseable message`);
    return;
  }
  logLine(`<- native: ${jsonText}`);

  switch (message.type) {
    case "runtime.ready":
      statusEl.textContent =
        `Connected: ${message.payload.plugin} v${message.payload.version} (${message.payload.game})`;
      break;
    case "runtime.pong":
      statusEl.textContent = "Pong received from native runtime.";
      break;
    default:
      // Unknown native messages are logged, never executed.
      break;
  }
}

// Publish the inbound handler where the native bridge will look for it.
window.prisma = window.prisma || {};
window.prisma.onMessage = onNativeMessage;

document.getElementById("ping").addEventListener("click", () => {
  sendToNative("ui.command", { command: "ping" });
});

document.getElementById("close").addEventListener("click", () => {
  sendToNative("ui.command", { command: "close" });
});

// Keyboard routing proof (Phase 4): show the last key the native side routed
// in, and keep the text field focused so typed characters land somewhere
// visible. The input's own value updating is the proof that kChar events
// reach the focused DOM element.
const textEl = document.getElementById("text");
const keysEl = document.getElementById("keys");

document.addEventListener("keydown", (e) => {
  keysEl.textContent = `last key: ${e.key} (code ${e.keyCode})`;
});

if (textEl) {
  // Re-assert focus so typing always has a target even if focus drifts.
  textEl.focus();
  textEl.addEventListener("blur", () => setTimeout(() => textEl.focus(), 0));
}

// Software pointer (Phase 4b): position our cursor div from routed mouse
// moves, since the OS cursor is hidden in-game.
const cursorEl = document.getElementById("cursor");
document.addEventListener("mousemove", (e) => {
  if (cursorEl) {
    cursorEl.style.left = `${e.clientX}px`;
    cursorEl.style.top = `${e.clientY}px`;
  }
});

if (bridgeAvailable()) {
  statusEl.textContent = "Bridge detected, waiting for runtime.ready…";
  // Automatic handshake: prove the web -> native -> web round trip without
  // any input wiring (mouse/keyboard routing into the view is Phase 4).
  sendToNative("ui.command", { command: "log", text: "test view loaded; bridge online" });
  sendToNative("ui.command", { command: "ping" });
} else {
  statusEl.textContent = "Standalone mode (no native bridge — opened in a browser?).";
  logLine("Native bridge not found; buttons will log locally only.");
}
