// Test panel logic. Talks to the native runtime exclusively through the
// narrow JSON message bridge described in docs/security-model.md.
//
// The native side will eventually expose:
//   window.osfui.postMessage(jsonString)   // web -> native
//   window.osfui.onMessage(jsonString)     // native -> web (assigned here)
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
  return typeof window.osfui === "object" &&
         typeof window.osfui.postMessage === "function";
}

function sendToNative(type, payload) {
  const message = JSON.stringify({ type, payload: payload ?? {} });
  if (bridgeAvailable()) {
    window.osfui.postMessage(message);
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
  // Stick telemetry is continuous — surface it in the keys line instead of
  // flooding the log; everything else is logged verbatim.
  const isStickEvent = message.type === "ui.gamepad" &&
                       message.payload && message.payload.kind === "stick";
  if (isStickEvent) {
    const p = message.payload;
    keysEl.textContent =
      `sticks: L(${p.lx.toFixed(2)}, ${p.ly.toFixed(2)}) R(${p.rx.toFixed(2)}, ${p.ry.toFixed(2)})`;
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
    case "ui.visibility":
      // The runtime sends this on the closed->open edge so the view can play
      // its entry treatment (the dim-backdrop fade; see style.css).
      if (message.payload && message.payload.visible) {
        playMenuFade();
      } else {
        document.body.classList.remove("osfui-shown");
      }
      break;
    default:
      // Unknown native messages are logged, never executed.
      break;
  }
}

// Replay the dim-backdrop fade each time the overlay is shown. Opening the
// overlay only un-hides a persistent page (no reload), so a CSS load animation
// would fire once at startup and never again; forcing a class reset + reflow
// restarts the transition on every open.
function playMenuFade() {
  document.body.classList.remove("osfui-shown");
  void document.body.offsetWidth;  // force reflow so the transition restarts
  document.body.classList.add("osfui-shown");
}

// Publish the inbound handler where the native bridge will look for it.
window.osfui = window.osfui || {};
window.osfui.onMessage = onNativeMessage;

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
  // Give typing a default target, but only reclaim focus when it was LOST
  // (fell back to <body>) — reclaiming unconditionally would fight the
  // gamepad-nav roving focus below.
  textEl.focus();
  textEl.addEventListener("blur", () => setTimeout(() => {
    if (document.activeElement === document.body) {
      textEl.focus();
    }
  }, 0));
}

// Gamepad-nav demo (Level 2): the native runtime maps D-pad / left stick to
// arrow keydowns and the A button to Enter, so plain keyboard-nav is all a
// page needs. Roving focus makes it visible: up/down moves across the
// interactive elements, Enter/A activates the focused button. Pages that want
// to OWN the pad instead send `osfui.gamepadRaw {raw:true}` on each show and
// read the raw `ui.gamepad` events.
const navOrder = [
  document.getElementById("ping"),
  document.getElementById("close"),
  textEl,
].filter(Boolean);
document.addEventListener("keydown", (e) => {
  if (e.key !== "ArrowUp" && e.key !== "ArrowDown") {
    return;
  }
  const idx = navOrder.indexOf(document.activeElement);  // -1 = start of list
  const step = e.key === "ArrowDown" ? 1 : -1;
  navOrder[(idx + step + navOrder.length) % navOrder.length].focus();
  e.preventDefault();
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
