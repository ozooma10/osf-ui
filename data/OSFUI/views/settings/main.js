// Schema-driven settings view — two-pane master/detail.
//
// Left rail lists the configurable subjects: OSF UI itself (the framework,
// pinned first) under FRAMEWORK, then every mod that ships a settings/<id>.json
// schema under MODS. The right pane renders the selected subject's typed
// controls on the shared OSF UI design system. Talks to the runtime only
// through the narrow JSON bridge (settings.get / settings.set / settings.reset);
// the native SettingsStore validates, clamps, persists, and reacts. This script
// is just the renderer.

"use strict";

// The framework's own settings mod id — pinned first, under FRAMEWORK.
const FRAMEWORK_ID = "osfui";

const statusEl = document.getElementById("status");
const detailEl = document.getElementById("detail");
const railEl = document.getElementById("rail-list");
const filterEl = document.getElementById("filter");

let allMods = [];
let selectedId = null;

function bridgeAvailable() {
  return typeof window.osfui === "object" &&
         typeof window.osfui.postMessage === "function";
}
function sendCommand(fields) {
  if (bridgeAvailable()) {
    window.osfui.postMessage(JSON.stringify({ type: "ui.command", payload: fields }));
  }
}
function setValue(modId, key, value) { sendCommand({ command: "settings.set", mod: modId, key, value }); }
function resetMod(modId) { sendCommand({ command: "settings.reset", mod: modId }); }

function escapeHtml(s) {
  return String(s).replace(/[&<>"']/g, (c) =>
    ({ "&": "&amp;", "<": "&lt;", ">": "&gt;", '"': "&quot;", "'": "&#39;" }[c]));
}
function initials(t) {
  const w = String(t).trim().split(/\s+/);
  if (w.length >= 2) return (w[0][0] + w[1][0]).toUpperCase();
  return w[0].replace(/[^A-Za-z0-9]/g, "").slice(0, 2).toUpperCase();
}
function titleOf(mod) { return mod.title || (mod.schema && mod.schema.title) || mod.id; }

// ---- control builders, one per schema type --------------------------------

function makeRow(setting, controlNode, valueNode) {
  const row = document.createElement("div");
  row.className = "row";
  row.dataset.label = (setting.label || setting.key).toLowerCase();

  const text = document.createElement("div");
  text.className = "row-text";
  const label = document.createElement("label");
  label.className = "row-label";
  label.textContent = setting.label || setting.key;
  label.htmlFor = `ctl-${setting.key}`;
  text.appendChild(label);
  if (setting.hint) {
    const hint = document.createElement("div");
    hint.className = "row-hint";
    hint.textContent = setting.hint;
    text.appendChild(hint);
  }
  row.appendChild(text);

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
      const sw = document.createElement("button");
      sw.type = "button"; sw.className = "osf-switch"; sw.id = id; sw.setAttribute("role", "switch");
      const set = (on) => sw.setAttribute("aria-pressed", on ? "true" : "false");
      set(current === true);
      sw.addEventListener("click", () => {
        const next = sw.getAttribute("aria-pressed") !== "true";
        set(next); setValue(modId, setting.key, next);
      });
      return makeRow(setting, sw, null);
    }
    case "int":
    case "float": {
      const isInt = setting.type === "int";
      const slider = document.createElement("input");
      slider.type = "range"; slider.className = "osf-range"; slider.id = id;
      slider.min = setting.min ?? 0; slider.max = setting.max ?? 100;
      slider.step = setting.step ?? (isInt ? 1 : 0.01); slider.value = current;
      const valueEl = document.createElement("span");
      valueEl.className = "osf-value";
      const fmt = (v) => (isInt ? String(Math.round(v)) : Number(v).toFixed(2));
      valueEl.textContent = fmt(current);
      slider.addEventListener("input", () => { valueEl.textContent = fmt(slider.value); });
      slider.addEventListener("change", () =>
        setValue(modId, setting.key, isInt ? parseInt(slider.value, 10) : parseFloat(slider.value)));
      return makeRow(setting, slider, valueEl);
    }
    case "enum": {
      const select = document.createElement("select");
      select.className = "osf-select"; select.id = id;
      for (const opt of setting.options || []) {
        const o = document.createElement("option");
        o.value = opt; o.textContent = opt;
        if (opt === current) o.selected = true;
        select.appendChild(o);
      }
      select.addEventListener("change", () => setValue(modId, setting.key, select.value));
      return makeRow(setting, select, null);
    }
    case "string": {
      const text = document.createElement("input");
      text.type = "text"; text.className = "osf-input"; text.id = id; text.value = current ?? "";
      text.addEventListener("change", () => setValue(modId, setting.key, text.value));
      return makeRow(setting, text, null);
    }
    case "key": {
      // A rebindable key. Clicking arms native capture (settings.captureKey);
      // the next key press comes back as settings.captured. Native does the
      // capture so pressing the CURRENT toggle key rebinds instead of closing
      // the overlay.
      const btn = document.createElement("button");
      btn.type = "button"; btn.className = "osf-btn osf-btn--sm osf-key"; btn.id = id;
      btn.textContent = current || "—";
      btn.addEventListener("click", () => beginCapture(modId, setting.key, btn));
      return makeRow(setting, btn, null);
    }
    default:
      return null;
  }
}

// ---- key rebind capture (one at a time) ----

let capturing = null;  // { mod, key, btn, prev }

function beginCapture(mod, key, btn) {
  if (capturing) return;
  capturing = { mod, key, btn, prev: btn.textContent };
  btn.classList.add("listening");
  btn.textContent = "Press a key…";
  if (bridgeAvailable()) {
    sendCommand({ command: "settings.captureKey", mod, key });  // native captures the next key
  } else {
    // Standalone (browser): capture a real DOM keydown so the flow is testable.
    const onKey = (e) => {
      window.removeEventListener("keydown", onKey, true);
      e.preventDefault();
      const name = domKeyName(e);
      finishCapture({ mod, key, name, cancelled: e.key === "Escape" || !name });
    };
    window.addEventListener("keydown", onKey, true);
  }
}

// Standalone-only: map a browser KeyboardEvent to an OSF UI key name (the
// in-game path does this natively via KeyName(vk)). Rough but enough to preview.
function domKeyName(e) {
  if (/^F([1-9]|1[0-9]|2[0-4])$/.test(e.key)) return e.key;            // F1..F24
  if (/^[a-z]$/i.test(e.key)) return e.key.toUpperCase();               // letters
  if (/^[0-9]$/.test(e.key)) return e.key;                              // digits
  const named = { " ": "Space", Enter: "Enter", Tab: "Tab", Backspace: "Backspace",
    Insert: "Insert", Delete: "Delete", Home: "Home", End: "End",
    PageUp: "PageUp", PageDown: "PageDown", ArrowUp: "Up", ArrowDown: "Down",
    ArrowLeft: "Left", ArrowRight: "Right", "`": "Grave" };
  return named[e.key] || "";
}

function finishCapture(payload) {
  if (!capturing) return;
  const { mod, key, btn, prev } = capturing;
  capturing = null;
  btn.classList.remove("listening");
  if (!payload || payload.cancelled || !payload.name) {
    btn.textContent = prev;  // Esc / unbindable key → keep the old binding
    return;
  }
  btn.textContent = payload.name;
  setValue(mod, key, payload.name);  // normal settings.set → persist + re-resolve
  // Keep the local model in sync so a re-render shows the new binding.
  const m = allMods.find((x) => x.id === mod);
  if (m) { m.values = m.values || {}; m.values[key] = payload.name; }
}

// ---- rendering ------------------------------------------------------------

function frameworkMods() { return allMods.filter((m) => m.id === FRAMEWORK_ID); }
function contentMods() { return allMods.filter((m) => m.id !== FRAMEWORK_ID); }

function railMatches(mod, q) {
  if (!q) return true;
  if (titleOf(mod).toLowerCase().includes(q)) return true;
  for (const g of (mod.schema && mod.schema.groups) || []) {
    for (const s of g.settings || []) {
      if ((s.label || s.key).toLowerCase().includes(q)) return true;
    }
  }
  return false;
}

function railItem(mod) {
  const btn = document.createElement("button");
  btn.type = "button";
  btn.className = "rail-item" + (mod.id === selectedId ? " selected" : "");
  btn.dataset.mod = mod.id;
  const isFramework = mod.id === FRAMEWORK_ID;
  btn.innerHTML =
    `<span class="rail-item-mark">${isFramework ? "◆" : escapeHtml(initials(titleOf(mod)))}</span>
     <span class="rail-item-text">
       <span class="rail-item-title">${escapeHtml(titleOf(mod))}</span>
       <span class="rail-item-sub">${isFramework ? "Framework" : escapeHtml(mod.id)}</span>
     </span>`;
  btn.addEventListener("click", () => selectMod(mod.id));
  return btn;
}

function renderRail() {
  const q = (filterEl.value || "").trim().toLowerCase();
  railEl.textContent = "";

  const fw = frameworkMods().filter((m) => railMatches(m, q));
  if (fw.length) {
    const head = document.createElement("div");
    head.className = "rail-section"; head.textContent = "Framework";
    railEl.appendChild(head);
    fw.forEach((m) => railEl.appendChild(railItem(m)));
  }

  const head = document.createElement("div");
  head.className = "rail-section"; head.textContent = "Mods";
  railEl.appendChild(head);
  const mods = contentMods().filter((m) => railMatches(m, q));
  if (mods.length) {
    mods.forEach((m) => railEl.appendChild(railItem(m)));
  } else {
    const empty = document.createElement("div");
    empty.className = "rail-empty";
    empty.innerHTML = q
      ? "No mods match the filter."
      : "No mods installed yet.<br>Drop a <code>settings/&lt;id&gt;.json</code> schema and it appears here.";
    railEl.appendChild(empty);
  }
}

function renderDetail() {
  const mod = allMods.find((m) => m.id === selectedId);
  detailEl.textContent = "";
  if (!mod) {
    const empty = document.createElement("div");
    empty.className = "detail-empty";
    empty.innerHTML = `<div class="osf-eyebrow">Nothing selected</div>`;
    detailEl.appendChild(empty);
    return;
  }
  const isFramework = mod.id === FRAMEWORK_ID;
  const schema = mod.schema || {};
  const values = mod.values || {};

  const head = document.createElement("div");
  head.className = "detail-head";
  head.innerHTML =
    `<div>
       <div class="osf-eyebrow kicker">${isFramework ? "Framework" : "Mod · " + escapeHtml(mod.id)}</div>
       <h2>${escapeHtml(titleOf(mod))}</h2>
       ${schema.description ? `<div class="detail-desc">${escapeHtml(schema.description)}</div>` : ""}
     </div>`;
  const reset = document.createElement("button");
  reset.type = "button";
  reset.className = "osf-btn osf-btn--danger osf-btn--sm";
  reset.textContent = "Reset";
  reset.addEventListener("click", () => resetMod(mod.id));
  head.appendChild(reset);
  detailEl.appendChild(head);

  const body = document.createElement("div");
  body.className = "detail-body";
  const q = (filterEl.value || "").trim().toLowerCase();
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
      if (!row) continue;
      // If a filter is active, show only matching rows (so the pane echoes the rail search).
      if (q && !row.dataset.label.includes(q) && !titleOf(mod).toLowerCase().includes(q)) {
        row.classList.add("hidden");
      }
      section.appendChild(row);
    }
    body.appendChild(section);
  }
  detailEl.appendChild(body);
}

function selectMod(id) {
  selectedId = id;
  renderRail();
  renderDetail();
}

function render() {
  if (!allMods.length) {
    railEl.textContent = "";
    detailEl.innerHTML = `<p class="status osf-eyebrow">No settings schemas found (settings/*.json).</p>`;
    return;
  }
  // Keep the current selection if it still exists; else default to the
  // framework, else the first mod.
  if (!allMods.some((m) => m.id === selectedId)) {
    selectedId = frameworkMods().length ? FRAMEWORK_ID : allMods[0].id;
  }
  renderRail();
  renderDetail();
}

filterEl.addEventListener("input", () => { renderRail(); renderDetail(); });

// ---- native -> web --------------------------------------------------------

function onNativeMessage(jsonText) {
  let message;
  try { message = JSON.parse(jsonText); } catch { return; }
  switch (message.type) {
    case "runtime.ready":
      sendCommand({ command: "settings.get" });
      break;
    case "settings.data":
      allMods = message.payload.mods || [];
      render();
      break;
    case "settings.captured":
      finishCapture(message.payload);
      break;
    case "settings.ack":
      if (!message.payload.ok) {
        statusEl && (statusEl.textContent = `REJECTED "${message.payload.mod}.${message.payload.key}"`);
      }
      break;
    default:
      break;
  }
}

window.osfui = window.osfui || {};
window.osfui.onMessage = onNativeMessage;

document.getElementById("close").addEventListener("click", () => sendCommand({ command: "close" }));

if (bridgeAvailable()) {
  sendCommand({ command: "settings.get" });
} else {
  // Standalone (plain browser) — sample schemas so the layout can be iterated.
  allMods = [
    {
      id: "osfui", title: "OSF UI",
      schema: {
        description: "Runtime and overlay behavior for the OSF UI framework itself.",
        groups: [
          { label: "Input", settings: [
            { key: "toggleKey", label: "Open / close key", type: "key", hint: "Press to rebind the key that opens and closes the overlay." },
          ] },
          { label: "Overlay", settings: [
            { key: "disableControls", label: "Disable player controls while open", type: "bool", hint: "Also freezes gamepad/XInput, which the window hook can't see." },
          ] },
          { label: "Cursor", settings: [
            { key: "cursorSpeed", label: "Cursor speed", type: "float", min: 0.5, max: 3.0, step: 0.1, hint: "Software-cursor fallback only." },
          ] },
        ],
      },
      values: { toggleKey: "F10", disableControls: true, cursorSpeed: 1.0 },
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
  ];
  render();
}
