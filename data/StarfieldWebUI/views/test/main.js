// Test panel logic. Talks to the native runtime exclusively through the
// narrow JSON message bridge described in docs/security-model.md.
//
// The native side will eventually expose:
//   window.starfield.postMessage(jsonString)   // web -> native
//   window.starfield.onMessage(jsonString)     // native -> web (assigned here)
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
  return typeof window.starfield === "object" &&
         typeof window.starfield.postMessage === "function";
}

function sendToNative(type, payload) {
  const message = JSON.stringify({ type, payload: payload ?? {} });
  if (bridgeAvailable()) {
    window.starfield.postMessage(message);
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
window.starfield = window.starfield || {};
window.starfield.onMessage = onNativeMessage;

document.getElementById("ping").addEventListener("click", () => {
  sendToNative("ui.command", { command: "ping" });
});

document.getElementById("close").addEventListener("click", () => {
  sendToNative("ui.command", { command: "close" });
});

if (bridgeAvailable()) {
  statusEl.textContent = "Bridge detected, waiting for runtime.ready…";
} else {
  statusEl.textContent = "Standalone mode (no native bridge — opened in a browser?).";
  logLine("Native bridge not found; buttons will log locally only.");
}
