// Schema-driven settings view (Phase 5). Talks to the native runtime only
// through the narrow JSON bridge: requests { schema, values }, renders typed
// controls, and sends one ui.command "settings.set" per change. The native
// SettingsStore validates/clamps/persists — this script is just the renderer.

"use strict";

const statusEl = document.getElementById("status");
const titleEl = document.getElementById("title");
const formEl = document.getElementById("form");
const cursorEl = document.getElementById("cursor");

function bridgeAvailable() {
  return typeof window.starfield === "object" &&
         typeof window.starfield.postMessage === "function";
}

function sendCommand(fields) {
  const message = JSON.stringify({ type: "ui.command", payload: fields });
  if (bridgeAvailable()) {
    window.starfield.postMessage(message);
  }
}

function setValue(key, value) {
  sendCommand({ command: "settings.set", key, value });
}

// ---- control builders, one per schema type ----

function makeRow(setting, controlNode, valueNode) {
  const row = document.createElement("div");
  row.className = "row";
  const label = document.createElement("label");
  label.textContent = setting.label || setting.key;
  label.htmlFor = `ctl-${setting.key}`;
  row.appendChild(label);
  const control = document.createElement("div");
  control.className = "control";
  if (valueNode) control.appendChild(valueNode);
  control.appendChild(controlNode);
  row.appendChild(control);
  return row;
}

function buildControl(setting, current) {
  const id = `ctl-${setting.key}`;
  switch (setting.type) {
    case "bool": {
      const cb = document.createElement("input");
      cb.type = "checkbox";
      cb.id = id;
      cb.checked = current === true;
      cb.addEventListener("change", () => setValue(setting.key, cb.checked));
      return makeRow(setting, cb, null);
    }
    case "int":
    case "float": {
      const isInt = setting.type === "int";
      const slider = document.createElement("input");
      slider.type = "range";
      slider.id = id;
      slider.min = setting.min ?? 0;
      slider.max = setting.max ?? 100;
      slider.step = setting.step ?? (isInt ? 1 : 0.01);
      slider.value = current;
      const valueEl = document.createElement("span");
      valueEl.className = "value";
      const fmt = (v) => (isInt ? String(Math.round(v)) : Number(v).toFixed(2));
      valueEl.textContent = fmt(current);
      slider.addEventListener("input", () => { valueEl.textContent = fmt(slider.value); });
      slider.addEventListener("change", () => {
        setValue(setting.key, isInt ? parseInt(slider.value, 10) : parseFloat(slider.value));
      });
      return makeRow(setting, slider, valueEl);
    }
    case "enum": {
      const select = document.createElement("select");
      select.id = id;
      for (const opt of setting.options || []) {
        const o = document.createElement("option");
        o.value = opt;
        o.textContent = opt;
        if (opt === current) o.selected = true;
        select.appendChild(o);
      }
      select.addEventListener("change", () => setValue(setting.key, select.value));
      return makeRow(setting, select, null);
    }
    case "string": {
      const text = document.createElement("input");
      text.type = "text";
      text.id = id;
      text.value = current ?? "";
      text.addEventListener("change", () => setValue(setting.key, text.value));
      return makeRow(setting, text, null);
    }
    default:
      return null;
  }
}

function render(schema, values) {
  formEl.textContent = "";
  if (schema && schema.title) titleEl.textContent = schema.title;

  const groups = (schema && schema.groups) || [];
  if (groups.length === 0) {
    statusEl.textContent = "No settings schema found (settings/schema.json).";
    return;
  }

  let count = 0;
  for (const group of groups) {
    const section = document.createElement("section");
    section.className = "group";
    const heading = document.createElement("div");
    heading.className = "group-label";
    heading.textContent = group.label || "";
    section.appendChild(heading);
    for (const setting of group.settings || []) {
      const row = buildControl(setting, values[setting.key]);
      if (row) { section.appendChild(row); count += 1; }
    }
    formEl.appendChild(section);
  }
  statusEl.textContent = `${count} setting(s) — changes save automatically.`;
}

// ---- native -> web ----

function onNativeMessage(jsonText) {
  let message;
  try { message = JSON.parse(jsonText); } catch { return; }
  switch (message.type) {
    case "runtime.ready":
      sendCommand({ command: "settings.get" });
      break;
    case "settings.data":
      render(message.payload.schema, message.payload.values || {});
      break;
    case "settings.ack":
      if (!message.payload.ok) {
        statusEl.textContent = `Rejected change to "${message.payload.key}".`;
      }
      break;
    default:
      break;
  }
}

window.starfield = window.starfield || {};
window.starfield.onMessage = onNativeMessage;

document.getElementById("close").addEventListener("click", () => {
  sendCommand({ command: "close" });
});

// Software pointer follows routed mouse moves (OS cursor hidden in-game).
document.addEventListener("mousemove", (e) => {
  if (cursorEl) {
    cursorEl.style.left = `${e.clientX}px`;
    cursorEl.style.top = `${e.clientY}px`;
  }
});

if (bridgeAvailable()) {
  statusEl.textContent = "Connecting…";
  // Ask immediately; also re-asked on runtime.ready in case we beat it.
  sendCommand({ command: "settings.get" });
} else {
  statusEl.textContent = "Standalone (no native bridge).";
}
