// Schema-driven settings view (Phase 5). Talks to the native runtime only
// through the narrow JSON bridge: requests the registry of mod schemas,
// renders typed controls for each mod, and sends one ui.command per change
// (settings.set) or reset (settings.reset). The native SettingsStore
// validates/clamps/persists and notifies native consumers — this script is
// just the renderer.

"use strict";

const statusEl = document.getElementById("status");
const titleEl = document.getElementById("title");
const formEl = document.getElementById("form");
const cursorEl = document.getElementById("cursor");

function bridgeAvailable() {
  return typeof window.osfui === "object" &&
         typeof window.osfui.postMessage === "function";
}

function sendCommand(fields) {
  if (bridgeAvailable()) {
    window.osfui.postMessage(JSON.stringify({ type: "ui.command", payload: fields }));
  }
}

function setValue(modId, key, value) {
  sendCommand({ command: "settings.set", mod: modId, key, value });
}

function resetMod(modId) {
  sendCommand({ command: "settings.reset", mod: modId });
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

function buildControl(modId, setting, current) {
  const id = `ctl-${setting.key}`;
  switch (setting.type) {
    case "bool": {
      const cb = document.createElement("input");
      cb.type = "checkbox";
      cb.id = id;
      cb.checked = current === true;
      cb.addEventListener("change", () => setValue(modId, setting.key, cb.checked));
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
        setValue(modId, setting.key, isInt ? parseInt(slider.value, 10) : parseFloat(slider.value));
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
      select.addEventListener("change", () => setValue(modId, setting.key, select.value));
      return makeRow(setting, select, null);
    }
    case "string": {
      const text = document.createElement("input");
      text.type = "text";
      text.id = id;
      text.value = current ?? "";
      text.addEventListener("change", () => setValue(modId, setting.key, text.value));
      return makeRow(setting, text, null);
    }
    default:
      return null;
  }
}

function buildMod(mod) {
  const schema = mod.schema || {};
  const values = mod.values || {};
  const card = document.createElement("section");
  card.className = "mod";

  const header = document.createElement("div");
  header.className = "mod-header";
  const h = document.createElement("h2");
  h.textContent = mod.title || schema.title || mod.id;
  header.appendChild(h);
  const reset = document.createElement("button");
  reset.type = "button";
  reset.className = "reset";
  reset.textContent = "Reset";
  reset.addEventListener("click", () => resetMod(mod.id));
  header.appendChild(reset);
  card.appendChild(header);

  let count = 0;
  for (const group of schema.groups || []) {
    const section = document.createElement("div");
    section.className = "group";
    if (group.label) {
      const heading = document.createElement("div");
      heading.className = "group-label";
      heading.textContent = group.label;
      section.appendChild(heading);
    }
    for (const setting of group.settings || []) {
      const row = buildControl(mod.id, setting, values[setting.key]);
      if (row) { section.appendChild(row); count += 1; }
    }
    card.appendChild(section);
  }
  return { card, count };
}

function render(mods) {
  formEl.textContent = "";
  if (!mods || mods.length === 0) {
    statusEl.textContent = "No settings schemas found (settings/*.json).";
    return;
  }
  let total = 0;
  for (const mod of mods) {
    const { card, count } = buildMod(mod);
    formEl.appendChild(card);
    total += count;
  }
  statusEl.textContent = `${mods.length} mod(s), ${total} setting(s) — changes save automatically.`;
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
      render(message.payload.mods || []);
      break;
    case "settings.ack":
      if (!message.payload.ok) {
        statusEl.textContent = `Rejected change to "${message.payload.mod}.${message.payload.key}".`;
      }
      break;
    default:
      break;
  }
}

window.osfui = window.osfui || {};
window.osfui.onMessage = onNativeMessage;

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
  sendCommand({ command: "settings.get" });
} else {
  statusEl.textContent = "Standalone (no native bridge).";
}
