// Schema-driven settings view. Talks to the native runtime only through the
// narrow JSON bridge: requests the registry of mod schemas, renders typed
// controls for each mod on the shared OSF UI design system, and sends one
// ui.command per change (settings.set) or reset (settings.reset). The native
// SettingsStore validates/clamps/persists and notifies native consumers — this
// script is just the renderer.

"use strict";

const statusEl = document.getElementById("status");
const formEl = document.getElementById("form");
const filterEl = document.getElementById("filter");

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
  row.dataset.label = (setting.label || setting.key).toLowerCase();

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
      // A real toggle switch (styled button) instead of a raw checkbox.
      const sw = document.createElement("button");
      sw.type = "button";
      sw.className = "osf-switch";
      sw.id = id;
      sw.setAttribute("role", "switch");
      const set = (on) => { sw.setAttribute("aria-pressed", on ? "true" : "false"); };
      set(current === true);
      sw.addEventListener("click", () => {
        const next = sw.getAttribute("aria-pressed") !== "true";
        set(next);
        setValue(modId, setting.key, next);
      });
      return makeRow(setting, sw, null);
    }
    case "int":
    case "float": {
      const isInt = setting.type === "int";
      const slider = document.createElement("input");
      slider.type = "range";
      slider.className = "osf-range";
      slider.id = id;
      slider.min = setting.min ?? 0;
      slider.max = setting.max ?? 100;
      slider.step = setting.step ?? (isInt ? 1 : 0.01);
      slider.value = current;
      const valueEl = document.createElement("span");
      valueEl.className = "osf-value";
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
      select.className = "osf-select";
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
      text.className = "osf-input";
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
  card.className = "mod osf-card";
  card.dataset.title = (mod.title || schema.title || mod.id).toLowerCase();

  const header = document.createElement("div");
  header.className = "mod-header";
  const h = document.createElement("h2");
  h.textContent = mod.title || schema.title || mod.id;
  header.appendChild(h);
  const reset = document.createElement("button");
  reset.type = "button";
  reset.className = "osf-btn osf-btn--danger osf-btn--sm";
  reset.textContent = "Reset";
  reset.addEventListener("click", () => resetMod(mod.id));
  header.appendChild(reset);
  card.appendChild(header);

  const body = document.createElement("div");
  body.className = "mod-body";

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
    body.appendChild(section);
  }
  card.appendChild(body);
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
  statusEl.textContent = `${mods.length} MOD(S) · ${total} SETTING(S) — CHANGES SAVE AUTOMATICALLY`;
  applyFilter();
}

// ---- filter ----
// Hides rows whose label doesn't match, and cards left with no visible rows.

function applyFilter() {
  const q = (filterEl.value || "").trim().toLowerCase();
  for (const card of formEl.querySelectorAll(".mod")) {
    let anyVisible = false;
    for (const row of card.querySelectorAll(".row")) {
      const match = !q || row.dataset.label.includes(q) || card.dataset.title.includes(q);
      row.classList.toggle("hidden", !match);
      if (match) anyVisible = true;
    }
    card.classList.toggle("hidden", !anyVisible);
  }
}
filterEl.addEventListener("input", applyFilter);

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
        statusEl.textContent = `REJECTED CHANGE TO "${message.payload.mod}.${message.payload.key}"`;
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

if (bridgeAvailable()) {
  statusEl.textContent = "CONNECTING…";
  sendCommand({ command: "settings.get" });
} else {
  // Standalone (plain browser) — render sample schemas so the layout can be
  // iterated without launching the game.
  statusEl.textContent = "STANDALONE (NO NATIVE BRIDGE)";
  render([
    {
      id: "osfui", title: "OSF UI Runtime",
      schema: { groups: [
        { label: "Cursor", settings: [
          { key: "cursorSpeed", label: "Cursor speed (fallback software cursor only)", type: "float", min: 0.5, max: 3.0, step: 0.1 },
        ] },
      ] },
      values: { cursorSpeed: 1.0 },
    },
    {
      id: "demo", title: "Demo Mod Settings",
      schema: { groups: [
        { label: "General", settings: [
          { key: "overlay.enabled", label: "Enable feature", type: "bool" },
          { key: "overlay.opacity", label: "Effect strength", type: "float", min: 0.2, max: 1.0, step: 0.05 },
          { key: "overlay.scale", label: "Amount (%)", type: "int", min: 50, max: 200, step: 5 },
        ] },
        { label: "Appearance", settings: [
          { key: "theme", label: "Theme", type: "enum", options: ["Dark", "Light", "Auto"] },
          { key: "greeting", label: "Greeting text", type: "string" },
        ] },
      ] },
      values: { "overlay.enabled": true, "overlay.opacity": 0.9, "overlay.scale": 100, theme: "Dark", greeting: "Hello, spacefarer" },
    },
  ]);
}
