// The Mods surface — two-pane master/detail, and the overlay's front door
// (the toggle key opens this view directly).
//
// The rail is topped by Home, the launcher page every fresh visit lands on:
// an app-style card grid of every registered panel (menu.open) and overlay
// toggle across all mods, so shipped surfaces read as launchable apps rather
// than settings rows. Below it the rail lists every installed mod: OSF UI
// itself (the framework) pinned first, then one entry per mod — the union of
// settings schemas
// (settings.data) and registered catalog views (views.data). A view names its
// owning mod via the manifest `mod` field; views without one get their own
// rail entry under the view's title. The right pane renders the selected
// mod's page: its panels (launched with menu.open — single-menu policy, so
// opening one replaces this surface) and HUD toggles (hud.show / hud.hide)
// first, then its typed settings controls on the shared OSF UI design system.
// Talks to the runtime only through the narrow JSON bridge (settings.* /
// views.get / menu.open / hud.*); the native SettingsStore validates, clamps,
// persists, and reacts. This script is just the renderer.
//
// Everything the schema adds beyond bool/int/float/enum/string/key is
// PRESENTATION: widget hints, number formatting, visibleWhen/enabledWhen
// conditions, note/image blocks, action buttons, requires badges, presets, the
// rail `icon` badge. The
// native store never trusts any of it — a hidden or disabled control is still
// validated on write, and an action command is refused unless it is namespaced
// with the owning mod's id. Untrusted schema text only ever reaches the DOM via
// textContent / createElement, never innerHTML.

"use strict";

// The framework's own settings mod id — pinned first in the rail.
const FRAMEWORK_ID = "osfui";
// The Home launcher page's rail id. "~" keeps it out of the mod-id namespace
// (native mod ids never start with it), so it can't shadow a real entry.
const HOME_ID = "~home";
// Bridge command namespaces owned by the framework — an action button may
// never fire into these (mirrors the reserved-id list in SettingsStore.cpp).
const RESERVED_NS = ["ui", "menu", "hud", "settings", "views", "game", "runtime"];
const ACTION_TIMEOUT_MS = 5000;
const HEX_RE = /^#([0-9a-fA-F]{6}|[0-9a-fA-F]{8})$/;
const COLOR_PRESETS = ["#5aa9b8", "#6fae6a", "#e0a23c", "#c8503a", "#c8607f", "#f0ece2", "#828a93", "#11151b"];

const statusEl = document.getElementById("status");
const detailEl = document.getElementById("detail");
const railEl = document.getElementById("rail-list");
const filterEl = document.getElementById("filter");
const toastEl = document.getElementById("toast");
const sessionChipEl = document.getElementById("session-chip");
const saveStateEl = document.getElementById("save-state");

let allMods = [];
// Registered catalog views (views.data, hub !== false) — panels and HUDs.
let allViews = [];
// Shape key of the last views.data (ids/owners/titles/loadState). A push that
// only flips open/focus state updates switches in place instead of tearing
// down the pane (views.data re-broadcasts on every such change).
let viewsShapeKey = "";
// Surface controls (panel buttons / HUD switches) of the current detail pane,
// reconciled in place by refreshSurfaceStates().
let surfaceControls = [];
// Dev nicety: #mod=<entry id> preselects a rail entry (harness deep-links /
// headless screenshots). In-game views load without a fragment, so it's inert.
// Held as "pending" until the entry exists — settings.data and views.data
// arrive in either order, and the default-selection fallback must not eat it.
const hashMod = /^#mod=(.+)$/.exec(location.hash || "");
let pendingHashSelect = hashMod ? decodeURIComponent(hashMod[1]) : null;
let selectedId = null;
// value when this settings VISIT began, baseline[modId][key] — drives the
// undo chip + revert panel. Seeded once per key on first change, kept across
// data refreshes (so a reset/preset re-broadcast doesn't lose undo history),
// and cleared on each fresh overlay open (ui.visibility) so the list scopes
// to "since you opened settings", not the whole game session. Nested (not a
// joined string key) so a key containing a space can't corrupt the split.
let baseline = {};
// Rebuilt every full render; consulted by refreshLive() to re-evaluate
// conditions and modified indicators without tearing the pane down.
let liveRows = [];
let liveGroups = [];
// Pending action buttons awaiting a "<mod>.ack": actionKey -> restore fn.
const pendingActions = new Map();

// ---- bridge ---------------------------------------------------------------

function bridgeAvailable() {
  return typeof window.osfui === "object" &&
         typeof window.osfui.postMessage === "function";
}
function sendCommand(fields) {
  if (bridgeAvailable()) {
    window.osfui.postMessage(JSON.stringify({ type: "ui.command", payload: fields }));
  }
}
function setValue(modId, key, value) { saveStatePending(modId); sendCommand({ command: "settings.set", mod: modId, key, value }); }
function resetMod(modId) { saveStatePending(modId); sendCommand({ command: "settings.reset", mod: modId }); }
function resetSetting(modId, key) { saveStatePending(modId); sendCommand({ command: "settings.reset", mod: modId, key }); }

// ---- save feedback ---------------------------------------------------------
// Native persistence is write-behind (a commit notifies immediately; the disk
// write lands ~0.5s later, coalesced per mod, guaranteed on menu close). Show
// "Saving…" from the moment we send a write until `settings.persisted`
// confirms every touched mod's file landed, then a fading "Saved". Persisted
// pushes for writes this view didn't make (a sibling DLL, another view) are
// deliberately ignored.
const pendingSaveMods = new Set();
let saveFadeTimer = 0;
function saveStatePending(modId) {
  if (!saveStateEl) return;
  pendingSaveMods.add(modId);
  clearTimeout(saveFadeTimer);
  saveStateEl.textContent = "Saving…";
  saveStateEl.classList.add("visible");
  saveStateEl.classList.remove("done");
}
function saveStatePersisted(modId) {
  if (!saveStateEl || !pendingSaveMods.delete(modId) || pendingSaveMods.size > 0) return;
  saveStateEl.textContent = "Saved";
  saveStateEl.classList.add("visible", "done");
  clearTimeout(saveFadeTimer);
  saveFadeTimer = setTimeout(() => { saveStateEl.classList.remove("visible", "done"); }, 1800);
}
function saveStateAbandon(modId) {
  // A rejected write never persists; don't leave "Saving…" stuck. If OTHER
  // changes to the same mod are still pending, its persisted push simply
  // won't find the entry — losing the confirmation, never showing a false one.
  if (!saveStateEl || !pendingSaveMods.delete(modId) || pendingSaveMods.size > 0) return;
  saveStateEl.classList.remove("visible", "done");
}

function baselineFor(modId) { return baseline[modId] || (baseline[modId] = {}); }

// Apply a value locally (optimistic) so conditions/dots update before the
// native ack, and record it against the session baseline.
function applyLocal(modId, key, value) {
  const mod = allMods.find((m) => m.id === modId);
  if (!mod) return;
  mod.values = mod.values || {};
  const b = baselineFor(modId);
  if (!(key in b)) b[key] = mod.values[key];
  mod.values[key] = value;
}

// A user commit: push to native, update the local model, refresh live state.
function commit(modId, key, value) {
  applyLocal(modId, key, value);
  setValue(modId, key, value);
  const mod = allMods.find((m) => m.id === modId);
  if (mod) refreshLive(mod);
}

// ---- utils ----------------------------------------------------------------

function initials(t) {
  const w = String(t).trim().split(/\s+/);
  if (w.length >= 2) return (w[0][0] + w[1][0]).toUpperCase();
  return w[0].replace(/[^A-Za-z0-9]/g, "").slice(0, 2).toUpperCase();
}
function titleOf(mod) { return mod.title || (mod.schema && mod.schema.title) || mod.id; }
const INPUT_CONTEXT_ID_RE = /^[A-Za-z0-9][A-Za-z0-9._-]{0,63}$/;
function inputContextFor(mod, setting) {
  const fallback = { id: "gameplay", label: "Gameplay", blocksGameplay: false };
  const ref = setting && typeof setting.inputContext === "string" ? setting.inputContext : "";
  if (!ref || ref === "gameplay" || !INPUT_CONTEXT_ID_RE.test(ref)) return fallback;
  const contexts = mod && mod.schema && Array.isArray(mod.schema.inputContexts)
    ? mod.schema.inputContexts : [];
  const seen = new Set();
  for (const context of contexts) {
    if (!context || typeof context !== "object") continue;
    const id = typeof context.id === "string" ? context.id : "";
    if (id === "gameplay" || !INPUT_CONTEXT_ID_RE.test(id) || seen.has(id)) continue;
    seen.add(id);
    if (id === ref) {
      return {
        id,
        label: typeof context.label === "string" && context.label ? context.label : id,
        blocksGameplay: context.blocksGameplay === true,
      };
    }
  }
  return fallback;
}
function el(tag, className, text) {
  const n = document.createElement(tag);
  if (className) n.className = className;
  if (text != null) n.textContent = text;
  return n;
}
function devWarn(msg) { if (typeof console !== "undefined" && console.warn) console.warn("[osfui settings] " + msg); }

function toast(message, kind) {
  if (!toastEl) return;
  const t = el("div", "toast" + (kind ? " toast--" + kind : ""), message);
  toastEl.appendChild(t);
  // Fade after a beat; localized to the toast node so the pane doesn't repaint.
  setTimeout(() => { t.classList.add("leaving"); }, 2600);
  setTimeout(() => { t.remove(); }, 3000);
}

// Micro-markdown -> DocumentFragment. Supports **bold**, *italic*, `code`, and
// \n line breaks. No HTML, no links — every literal goes through textContent.
function renderInline(text) {
  const frag = document.createDocumentFragment();
  const lines = String(text).split("\n");
  lines.forEach((line, i) => {
    if (i > 0) frag.appendChild(document.createElement("br"));
    const re = /(\*\*([^*]+)\*\*)|(\*([^*]+)\*)|(`([^`]+)`)/g;
    let last = 0, m;
    while ((m = re.exec(line)) !== null) {
      if (m.index > last) frag.appendChild(document.createTextNode(line.slice(last, m.index)));
      if (m[2] != null) frag.appendChild(el("strong", null, m[2]));
      else if (m[4] != null) frag.appendChild(el("em", null, m[4]));
      else if (m[6] != null) frag.appendChild(el("code", null, m[6]));
      last = re.lastIndex;
    }
    if (last < line.length) frag.appendChild(document.createTextNode(line.slice(last)));
  });
  return frag;
}

// int/float display: store the raw value, show a friendly string.
function formatNumber(setting, v) {
  const f = setting.format || {};
  const scale = typeof f.scale === "number" ? f.scale : 1;
  const n = Number(v) * scale;
  let s;
  // Clamp decimals to [0,20]: toFixed throws RangeError beyond the engine limit
  // (only 20 on older WebKit/JSCore), which would blank the whole detail pane.
  if (typeof f.decimals === "number") s = n.toFixed(Math.min(20, Math.max(0, f.decimals | 0)));
  else if (setting.type === "int") s = String(Math.round(n));
  else s = Number(n).toFixed(2);
  return (f.prefix || "") + s + (f.suffix || "");
}

function optionLabel(setting, opt) {
  const opts = setting.options || [];
  const labels = setting.optionLabels || [];
  const idx = opts.indexOf(opt);
  return (idx >= 0 && labels[idx] != null) ? labels[idx] : opt;
}

// ---- conditions + modified state ------------------------------------------

function evalCondition(cond, values) {
  if (!cond || typeof cond !== "object") return true;
  if (Array.isArray(cond.all)) return cond.all.every((c) => evalCondition(c, values));
  if (Array.isArray(cond.any)) return cond.any.some((c) => evalCondition(c, values));
  if (cond.not) return !evalCondition(cond.not, values);
  if (typeof cond.key === "string") {
    if (!(cond.key in values)) { devWarn(`condition references unknown key "${cond.key}"`); return false; }
    const v = values[cond.key];
    if ("eq" in cond) return v === cond.eq;
    if ("ne" in cond) return v !== cond.ne;
    if ("in" in cond) return Array.isArray(cond.in) && cond.in.includes(v);
    if ("gt" in cond) return Number(v) > cond.gt;
    if ("gte" in cond) return Number(v) >= cond.gte;
    if ("lt" in cond) return Number(v) < cond.lt;
    if ("lte" in cond) return Number(v) <= cond.lte;
    if ("truthy" in cond) return cond.truthy ? !!v : !v;
    return true;
  }
  return true;
}

function isSetting(item) {
  return item && (item.type === "bool" || item.type === "int" || item.type === "float" ||
    item.type === "enum" || item.type === "string" || item.type === "key");
}
// The schema setting object for a mod's key, or null.
function findSettingInMod(mod, key) {
  for (const g of (mod.schema && mod.schema.groups) || []) {
    for (const s of g.settings || []) { if (s && s.key === key) return s; }
  }
  return null;
}
function isModified(setting, value) {
  if (value === undefined || !("default" in setting)) return false;
  return value !== setting.default;
}
function modifiedCount(mod) {
  let n = 0;
  for (const g of (mod.schema && mod.schema.groups) || []) {
    for (const s of g.settings || []) {
      if (isSetting(s) && isModified(s, (mod.values || {})[s.key])) n++;
    }
  }
  return n;
}
function sessionChangeCount() {
  let n = 0;
  for (const modId in baseline) {
    const mod = allMods.find((m) => m.id === modId);
    if (!mod) continue;
    const values = mod.values || {};
    for (const key in baseline[modId]) {
      if (values[key] !== baseline[modId][key]) n++;
    }
  }
  return n;
}

// ---- control builders ------------------------------------------------------

function buildBool(modId, setting, id, current) {
  const sw = document.createElement("button");
  sw.type = "button"; sw.className = "osf-switch"; sw.id = id; sw.setAttribute("role", "switch");
  const set = (on) => sw.setAttribute("aria-pressed", on ? "true" : "false");
  set(current === true);
  sw.addEventListener("click", () => {
    const next = sw.getAttribute("aria-pressed") !== "true";
    set(next); commit(modId, setting.key, next);
  });
  return { control: sw, value: null };
}

function buildRange(modId, setting, id, current) {
  const isInt = setting.type === "int";
  const stepper = setting.widget === "stepper";
  const min = setting.min ?? 0, max = setting.max ?? 100;
  // A schema step of 0/negative/NaN would divide-by-zero in the stepper's snap
  // (NaN committed over the bridge) — fall back to the type default.
  let step = setting.step ?? (isInt ? 1 : 0.01);
  if (!(step > 0)) { devWarn(`"${setting.key}" has invalid step ${setting.step}`); step = isInt ? 1 : 0.01; }
  let val = current ?? min;

  const valueEl = el("span", "osf-value", formatNumber(setting, val));

  if (stepper) {
    const wrap = el("div", "osf-stepper");
    const dec = el("button", "osf-stepper-btn", "−"); dec.type = "button";
    const inc = el("button", "osf-stepper-btn", "+"); inc.type = "button";
    const read = el("span", "osf-stepper-val", formatNumber(setting, val));
    const clamp = (v) => Math.min(max, Math.max(min, v));
    // Snap to the step grid relative to min (so an off-grid start still snaps),
    // and round away IEEE drift so repeated float steps stay comparable to the
    // schema default (e.g. 1.2, not 1.2000000000000002).
    const snap = (v) => {
      let s = min + Math.round((v - min) / step) * step;
      return isInt ? Math.round(s) : Math.round(s * 1e6) / 1e6;
    };
    const apply = (v) => {
      val = clamp(snap(v));
      read.textContent = formatNumber(setting, val);
      commit(modId, setting.key, val);
    };
    dec.addEventListener("click", () => apply(val - step));
    inc.addEventListener("click", () => apply(val + step));
    wrap.append(dec, read, inc);
    wrap.id = id;
    return { control: wrap, value: null };
  }

  const slider = document.createElement("input");
  slider.type = "range"; slider.className = "osf-range"; slider.id = id;
  slider.min = min; slider.max = max; slider.step = step; slider.value = val;
  slider.addEventListener("input", () => { valueEl.textContent = formatNumber(setting, slider.value); });
  slider.addEventListener("change", () =>
    commit(modId, setting.key, isInt ? parseInt(slider.value, 10) : parseFloat(slider.value)));
  return { control: slider, value: valueEl };
}

function buildEnum(modId, setting, id, current) {
  const opts = setting.options || [];
  if (setting.widget === "segmented" && opts.length && opts.length <= 5) {
    const seg = el("div", "osf-segmented"); seg.id = id; seg.setAttribute("role", "group");
    const buttons = [];
    const select = (opt) => {
      buttons.forEach((b) => b.setAttribute("aria-pressed", b.dataset.opt === opt ? "true" : "false"));
    };
    for (const opt of opts) {
      const b = el("button", "osf-segment", optionLabel(setting, opt));
      b.type = "button"; b.dataset.opt = opt;
      b.addEventListener("click", () => { select(opt); commit(modId, setting.key, opt); });
      buttons.push(b); seg.appendChild(b);
    }
    select(current);
    return { control: seg, value: null };
  }
  const select = document.createElement("select");
  select.className = "osf-select"; select.id = id;
  for (const opt of opts) {
    const o = document.createElement("option");
    o.value = opt; o.textContent = optionLabel(setting, opt);
    if (opt === current) o.selected = true;
    select.appendChild(o);
  }
  select.addEventListener("change", () => commit(modId, setting.key, select.value));
  return { control: select, value: null };
}

function buildString(modId, setting, id, current) {
  if (setting.widget === "color") return buildColor(modId, setting, id, current);
  // Native SettingsStore currently hard-caps strings at 256; the native slice
  // raises this (and honors per-setting maxLength) up to 4096 — bump both in
  // lockstep then. Until then, clamp here so the UI can't accept text the store
  // would silently truncate.
  const maxLength = Math.min(256, setting.maxLength || 256);
  if (setting.widget === "textarea") {
    const ta = document.createElement("textarea");
    ta.className = "osf-input osf-textarea"; ta.id = id; ta.rows = 3;
    ta.maxLength = maxLength; ta.value = current ?? "";
    ta.addEventListener("change", () => commit(modId, setting.key, ta.value));
    return { control: ta, value: null };
  }
  const text = document.createElement("input");
  text.type = "text"; text.className = "osf-input"; text.id = id;
  text.maxLength = maxLength; text.value = current ?? "";
  text.addEventListener("change", () => commit(modId, setting.key, text.value));
  return { control: text, value: null };
}

function buildColor(modId, setting, id, current) {
  const wrap = el("div", "osf-color"); wrap.id = id;
  const swatch = el("span", "osf-color-swatch");
  const hex = document.createElement("input");
  hex.type = "text"; hex.className = "osf-input osf-color-hex"; hex.value = current || "";
  hex.spellcheck = false; hex.maxLength = 9;
  const paint = (v) => { swatch.style.background = HEX_RE.test(v) ? v : "transparent"; };
  paint(current || "");
  const applyHex = () => {
    const v = hex.value.trim();
    // Track the last committed colour so an invalid-input revert restores it,
    // not the session-start value.
    if (HEX_RE.test(v)) { current = v; paint(v); commit(modId, setting.key, v); }
    else { hex.value = current || ""; paint(hex.value); toast("Enter a hex colour like #5aa9b8", "warn"); }
  };
  hex.addEventListener("change", applyHex);
  const presets = el("div", "osf-color-presets");
  for (const p of COLOR_PRESETS) {
    const b = el("button", "osf-color-preset"); b.type = "button";
    b.style.background = p; b.title = p;
    b.addEventListener("click", () => { hex.value = p; current = p; paint(p); commit(modId, setting.key, p); });
    presets.appendChild(b);
  }
  wrap.append(swatch, hex, presets);
  return { control: wrap, value: null };
}

function buildKey(modId, setting, id, current) {
  // A rebindable key. Clicking arms native capture (settings.captureKey); the
  // next key press comes back as settings.captured. Native does the capture so
  // pressing the CURRENT toggle key rebinds instead of closing the overlay.
  const btn = document.createElement("button");
  btn.type = "button"; btn.className = "osf-btn osf-btn--sm osf-key"; btn.id = id;
  btn.textContent = current || "—";
  btn.addEventListener("click", () => beginCapture(modId, setting.key, btn));
  if (!setting.allowUnbound || !current) {
    return { control: btn, value: null };
  }
  // Schema opted into the unbound state ("" — skipped by hotkey dispatch and
  // conflicts). The ✕ commits ""; the key-rebind refresh path re-renders us.
  const wrap = el("span", "osf-key-wrap");
  wrap.appendChild(btn);
  const clear = document.createElement("button");
  clear.type = "button"; clear.className = "osf-btn osf-btn--sm osf-btn--ghost osf-key-clear";
  clear.textContent = "×";
  clear.title = "Unbind";
  clear.setAttribute("aria-label", `Unbind ${setting.label || setting.key}`);
  clear.addEventListener("click", () => commit(modId, setting.key, ""));
  wrap.appendChild(clear);
  return { control: wrap, value: null };
}

function buildSettingControl(modId, setting, id, current) {
  switch (setting.type) {
    case "bool": return buildBool(modId, setting, id, current);
    case "int":
    case "float": return buildRange(modId, setting, id, current);
    case "enum": return buildEnum(modId, setting, id, current);
    case "string": return buildString(modId, setting, id, current);
    case "key": return buildKey(modId, setting, id, current);
    default: return null;
  }
}

// ---- rows ------------------------------------------------------------------

const REQUIRES_LABEL = { restart: "Restart", reload: "Reload UI", newGame: "New game" };

function makeSettingRow(mod, setting, current) {
  if (typeof setting.key !== "string" || !setting.key) {
    // The store keeps schemas verbatim; a keyless setting can't be committed
    // (or even labelled) — skip it rather than blank the whole pane.
    devWarn(`skipping a "${setting.type}" setting with no key in "${mod.id}"`);
    return null;
  }
  // Prefix the mod id so the same key in two mods can't yield duplicate DOM
  // ids (which would break label[for] association).
  const id = `ctl-${mod.id}-${setting.key}`;
  const built = buildSettingControl(mod.id, setting, id, current);
  if (!built) {
    // Unknown/newer type: render read-only so a stale runtime degrades cleanly.
    return unknownRow(setting);
  }

  const row = el("div", "row");
  row.dataset.label = (setting.label || setting.key || "").toLowerCase();
  row.dataset.key = setting.key;

  const text = el("div", "row-text");
  const labelWrap = el("div", "row-label-line");
  const label = document.createElement("label");
  label.className = "row-label"; label.textContent = setting.label || setting.key; label.htmlFor = id;
  const dot = el("span", "osf-dot"); dot.title = "Changed from default";
  if (isModified(setting, current)) dot.classList.add("on");
  labelWrap.append(label, dot);
  if (setting.requires && REQUIRES_LABEL[setting.requires]) {
    labelWrap.appendChild(el("span", "osf-badge osf-badge--warn", REQUIRES_LABEL[setting.requires]));
  }
  if (setting.type === "key") {
    const context = inputContextFor(mod, setting);
    if (context.id !== "gameplay") {
      const badge = el("span", "osf-badge", context.label);
      badge.title = context.blocksGameplay
        ? "Active in this context; Starfield gameplay bindings are unavailable."
        : "Active in this input context.";
      labelWrap.appendChild(badge);
    }
  }
  // Key-binding conflict (mcm-design §9): native embeds `conflicts:[{mod,key,
  // title}]` on a key setting whose resolved key is also bound elsewhere.
  // Informational — the bind stands; we just badge both sides.
  if (setting.type === "key" && Array.isArray(setting.conflicts) && setting.conflicts.length) {
    const others = [...new Set(setting.conflicts.map((c) => c.title || c.mod))];
    const badge = el("span", "osf-badge osf-badge--stop", "Key conflict");
    badge.title = "Also bound by: " + others.join(", ");
    labelWrap.appendChild(badge);
  }
  text.appendChild(labelWrap);
  if (setting.hint) text.appendChild(el("div", "row-hint", setting.hint));
  row.appendChild(text);

  const control = el("div", "control");
  if (built.value) control.appendChild(built.value);
  control.appendChild(built.control);
  // Per-setting reset — appears on hover/when modified.
  const reset = el("button", "row-reset", "↺");
  reset.type = "button"; reset.title = "Reset to default";
  reset.addEventListener("click", () => resetSetting(mod.id, setting.key));
  control.appendChild(reset);
  row.appendChild(control);

  liveRows.push({ row, control: built.control, setting, dot,
    visibleWhen: setting.visibleWhen, enabledWhen: setting.enabledWhen, requires: setting.requires });
  return row;
}

function unknownRow(setting) {
  const row = el("div", "row row--unknown");
  row.dataset.label = (setting.label || setting.key || "").toLowerCase();
  const text = el("div", "row-text");
  text.appendChild(el("div", "row-label", setting.label || setting.key || "(setting)"));
  text.appendChild(el("div", "row-hint", `Type "${setting.type}" needs a newer OSF UI.`));
  row.appendChild(text);
  return row;
}

function buildNote(item) {
  // Whitelist the style — item.style is untrusted schema text that lands in the
  // class list; anything outside the enum could inject arbitrary CSS classes.
  const style = ["info", "warn", "danger"].includes(item.style) ? item.style : "info";
  const note = el("div", "osf-note osf-note--" + style);
  note.appendChild(renderInline(item.text || ""));
  liveRows.push({ row: note, control: null, setting: null, dot: null, visibleWhen: item.visibleWhen });
  return note;
}

function buildImage(mod, item) {
  const fig = el("figure", "osf-figure");
  const src = safeAssetSrc(mod.id, item.src);
  if (src) {
    const img = document.createElement("img");
    img.className = "osf-image"; img.src = src; img.alt = item.caption || "";
    if (item.height) img.style.maxHeight = (item.height | 0) + "px";
    fig.appendChild(img);
  } else {
    fig.appendChild(el("div", "osf-note osf-note--warn", "Image path rejected (must be inside the mod's view folder)."));
  }
  if (item.caption) fig.appendChild(el("figcaption", "osf-figcaption", item.caption));
  liveRows.push({ row: fig, control: null, setting: null, dot: null, visibleWhen: item.visibleWhen });
  return fig;
}

// Confine an image to the mod's own views/<id>/ folder: reject "..", absolute
// paths, URL schemes, and percent-encoding (which WebKit would decode back into
// "../" or a scheme after this check) before the src reaches the sandbox.
function safeAssetSrc(modId, src) {
  const s = String(src || "");
  if (!s) return null;
  let decoded = s;
  try { decoded = decodeURIComponent(s); } catch { return null; }
  const bad = (v) => v.includes("..") || /^[a-z]+:/i.test(v) || v.startsWith("/") || v.startsWith("\\");
  // The mod id is interpolated into the path too — hold it to the same rules
  // (the store sanitizes ids, but this renderer also runs against mock data).
  const id = String(modId || "");
  if (!id || id.includes("%") || bad(id)) return null;
  if (s.includes("%") || bad(s) || bad(decoded)) return null;
  // In game every view mounts under one views/ root, so a mod's folder is
  // always a sibling at "..". The dev harness serves this page from
  // devtools/harness/ where that isn't true — its mock bridge maps mod ids
  // to their real view folders (OSFUI_MOD_ASSET_ROOTS). Never defined in
  // game, so the shipped path can't be redirected.
  const roots = window.OSFUI_MOD_ASSET_ROOTS;
  const root = roots && typeof roots[id] === "string" ? roots[id] : "..";
  return `${root}/${id}/${s}`;
}

function buildAction(mod, item) {
  const row = el("div", "row row--action");
  row.dataset.label = (item.label || item.key || "").toLowerCase();
  const textWrap = el("div", "row-text");
  textWrap.appendChild(el("div", "row-label", item.label || item.key || "(action)"));
  if (item.hint) textWrap.appendChild(el("div", "row-hint", item.hint));
  row.appendChild(textWrap);

  const control = el("div", "control");
  const style = item.style === "accent" ? " osf-btn--accent" : item.style === "danger" ? " osf-btn--danger" : "";
  const btn = el("button", "osf-btn osf-btn--sm" + style, item.label || "Run");
  btn.type = "button";

  const fire = () => {
    if (typeof item.command !== "string" || !item.command.startsWith(mod.id + ".")) {
      toast(`Action refused: "${item.command}" is not namespaced to ${mod.id}`, "danger");
      return;
    }
    // Framework namespaces are never a mod's to fire, even if a schema claims
    // one as its id (the store rejects those ids too — defense stays layered).
    const ns = item.command.slice(0, item.command.indexOf("."));
    if (RESERVED_NS.includes(ns)) {
      toast(`Action refused: "${ns}." is a reserved framework namespace`, "danger");
      return;
    }
    btn.disabled = true; btn.classList.add("pending"); const prev = btn.textContent; btn.textContent = "…";
    // Key the pending map by mod+action key — action keys are only unique
    // within a mod, so two mods can share one.
    const pkey = mod.id + " " + item.key;
    // Identity check, not just presence: a stale timer from a previous fire
    // must not cancel a re-clicked action's fresh pending entry.
    const restore = (msg, kind) => {
      if (pendingActions.get(pkey) !== restore) return;
      pendingActions.delete(pkey);
      btn.disabled = false; btn.classList.remove("pending"); btn.textContent = prev;
      if (msg) toast(msg, kind);
    };
    pendingActions.set(pkey, restore);
    setTimeout(() => restore("No response from " + mod.id, "warn"), ACTION_TIMEOUT_MS);
    sendCommand(Object.assign({ command: item.command }, { mod: mod.id, key: item.key }));
  };

  if (item.confirm) {
    btn.addEventListener("click", () => askConfirm(control, btn, item.confirm, fire));
  } else {
    btn.addEventListener("click", fire);
  }
  control.appendChild(btn);
  row.appendChild(control);
  liveRows.push({ row, control: btn, setting: null, dot: null,
    visibleWhen: item.visibleWhen, enabledWhen: item.enabledWhen });
  return row;
}

// Inline confirm: swap the button for a confirm/cancel pair (no native dialog).
function askConfirm(control, btn, message, onConfirm) {
  btn.style.display = "none";
  const box = el("div", "confirm");
  box.appendChild(el("span", "confirm-msg", message));
  const yes = el("button", "osf-btn osf-btn--sm osf-btn--danger", "Confirm"); yes.type = "button";
  const no = el("button", "osf-btn osf-btn--sm osf-btn--ghost", "Cancel"); no.type = "button";
  const close = () => { box.remove(); btn.style.display = ""; };
  yes.addEventListener("click", () => { close(); onConfirm(); });
  no.addEventListener("click", close);
  box.append(yes, no);
  control.appendChild(box);
}

function buildItem(mod, item, values) {
  if (item && item.type === "note") return buildNote(item);
  if (item && item.type === "image") return buildImage(mod, item);
  if (item && item.type === "action") return buildAction(mod, item);
  return makeSettingRow(mod, item, values[item.key]);
}

// ---- rail (one entry per installed mod) --------------------------------------
// An entry is { id, mod, views, title }: `mod` is the settings.data record (or
// null for a view-only mod), `views` the catalog views attached to it. Views
// attach to a settings mod when their manifest `mod` matches its id; the rest
// group by their own `mod` string (or view id) into a view-only entry.

function railEntries() {
  const entries = allMods.map((m) => ({
    id: m.id, mod: m, views: allViews.filter((v) => v.mod === m.id), title: titleOf(m),
  }));
  const orphans = new Map();
  for (const v of allViews) {
    if (v.mod && allMods.some((m) => m.id === v.mod)) continue;
    const key = v.mod || v.id;
    if (!orphans.has(key)) orphans.set(key, []);
    orphans.get(key).push(v);
  }
  for (const [key, views] of orphans) {
    // A view-only mod has no schema title; borrow its (first) panel's title.
    const lead = views.find((v) => v.kind === "menu") || views[0];
    entries.push({ id: "view:" + key, mod: null, views, title: lead.title || key });
  }
  return entries;
}
function findEntry(id) { return railEntries().find((e) => e.id === id); }

function railMatches(entry, q) {
  if (!q) return true;
  if (entry.title.toLowerCase().includes(q)) return true;
  if (entry.views.some((v) => (v.title || "").toLowerCase().includes(q))) return true;
  for (const g of (entry.mod && entry.mod.schema && entry.mod.schema.groups) || []) {
    for (const s of g.settings || []) {
      if (((s.label || s.key || "")).toLowerCase().includes(q)) return true;
    }
  }
  return false;
}

// What the entry offers, for the rail sub-line: "Framework", the mod id, or a
// terminal/overlay summary for view-only entries.
function railSub(entry) {
  if (entry.id === FRAMEWORK_ID) return "Framework";
  if (entry.mod) return entry.mod.id;
  const menus = entry.views.filter((v) => v.kind === "menu").length;
  const huds = entry.views.length - menus;
  const parts = [];
  if (menus) parts.push(menus === 1 ? "Terminal" : `${menus} terminals`);
  if (huds) parts.push(huds === 1 ? "Overlay" : `${huds} overlays`);
  return parts.join(" · ") || "Mod";
}

// A mod's schema `icon` (a file inside its own view folder — same sandbox as
// image rows) resolved to an URL, or null when it ships none / the path is
// rejected.
function modIconSrc(mod) {
  const icon = mod && mod.schema ? mod.schema.icon : null;
  return typeof icon === "string" ? safeAssetSrc(mod.id, icon) : null;
}
// Same, looked up from a view's owning-mod id (Home cards).
function ownerIconSrc(modId) {
  return modId ? modIconSrc(allMods.find((m) => m.id === modId)) : null;
}

// The rail avatar: the mod's schema `icon` when it ships one, else title
// initials. A missing/broken file falls back to initials, so a stale path
// never leaves a hole in the rail.
function railMark(entry, isFramework) {
  const mark = el("span", "rail-item-mark");
  const fallback = isFramework ? "◆" : initials(entry.title);
  const src = modIconSrc(entry.mod);
  if (!src) { mark.textContent = fallback; return mark; }
  mark.classList.add("rail-item-mark--icon");
  const img = document.createElement("img");
  img.src = src;
  img.alt = "";
  img.addEventListener("error", () => {
    img.remove();
    mark.classList.remove("rail-item-mark--icon");
    mark.textContent = fallback;
  });
  mark.appendChild(img);
  return mark;
}

function railItem(entry) {
  const btn = document.createElement("button");
  btn.type = "button";
  btn.className = "rail-item" + (entry.id === selectedId ? " selected" : "");
  btn.dataset.mod = entry.id;
  const isFramework = entry.id === FRAMEWORK_ID;

  const mark = railMark(entry, isFramework);
  const textWrap = el("span", "rail-item-text");
  textWrap.appendChild(el("span", "rail-item-title", entry.title));
  textWrap.appendChild(el("span", "rail-item-sub", railSub(entry)));
  btn.append(mark, textWrap);

  const count = entry.mod ? modifiedCount(entry.mod) : 0;
  if (count) {
    const badge = el("span", "rail-item-count", String(count));
    badge.title = `${count} changed from default`;
    btn.appendChild(badge);
  }
  btn.addEventListener("click", () => selectMod(entry.id));
  return btn;
}

// Home's pinned rail item — same chrome as a mod entry, selected the same way.
function railHomeItem() {
  const btn = document.createElement("button");
  btn.type = "button";
  btn.className = "rail-item rail-item--home" + (selectedId === HOME_ID ? " selected" : "");
  const mark = el("span", "rail-item-mark", "◉");
  const textWrap = el("span", "rail-item-text");
  textWrap.appendChild(el("span", "rail-item-title", "Systems"));
  const menus = allViews.filter((v) => v.kind === "menu").length;
  const huds = allViews.length - menus;
  textWrap.appendChild(el("span", "rail-item-sub",
    allViews.length ? `${menus} terminals · ${huds} overlays` : "Standby"));
  btn.append(mark, textWrap);
  btn.addEventListener("click", () => selectMod(HOME_ID));
  return btn;
}

function renderRail() {
  const q = (filterEl.value || "").trim().toLowerCase();
  railEl.textContent = "";

  // Home is pinned above everything; while a filter is active the rail scopes
  // to matching mods, so the launcher item steps aside like the framework does.
  if (!q) railEl.appendChild(railHomeItem());

  // The framework card is pinned at the top with no section header of its own
  // — it self-labels as "Framework" and is the only entry that would ever sit
  // under such a header. The "Mods" header below is what separates it from
  // installed mods.
  const entries = railEntries();
  entries.filter((e) => e.id === FRAMEWORK_ID && railMatches(e, q))
    .forEach((e) => railEl.appendChild(railItem(e)));

  railEl.appendChild(el("div", "rail-section", "Mods"));
  const mods = entries.filter((e) => e.id !== FRAMEWORK_ID && railMatches(e, q))
    .sort((a, b) => a.title.localeCompare(b.title, undefined, { sensitivity: "base" }));
  if (mods.length) {
    mods.forEach((e) => railEl.appendChild(railItem(e)));
  } else {
    const empty = el("div", "rail-empty");
    if (q) {
      empty.textContent = "No mods match the filter.";
    } else {
      empty.textContent = "No mods installed yet. Mods that register settings, terminals or overlays appear here.";
    }
    railEl.appendChild(empty);
  }
}

// ---- detail ----------------------------------------------------------------

// The four linked accent tokens the design system uses together. A schema
// gives us one hex; most accent surfaces (eyebrows, readouts, hovers, quiet
// backgrounds) read the DERIVED three, so set all four or the accent only
// half-applies. Cleared as a set so nothing leaks onto the next mod.
const ACCENT_TOKENS = ["--accent", "--accent-hover", "--accent-quiet", "--accent-strong"];
function hexToRgb(hex) {
  return [parseInt(hex.slice(1, 3), 16), parseInt(hex.slice(3, 5), 16), parseInt(hex.slice(5, 7), 16)];
}
// Blend each channel toward a target (255 = lighten, 0 = darken) by t in [0,1].
function mixToward(rgb, target, t) {
  return rgb.map((c) => Math.round(c + (target - c) * t));
}
function rgbToHex(rgb) {
  return "#" + rgb.map((c) => Math.max(0, Math.min(255, c)).toString(16).padStart(2, "0")).join("");
}
function applyAccent(node, accent) {
  if (typeof accent === "string" && HEX_RE.test(accent)) {
    const rgb = hexToRgb(accent);
    node.style.setProperty("--accent", accent);
    node.style.setProperty("--accent-hover", rgbToHex(mixToward(rgb, 255, 0.34)));
    node.style.setProperty("--accent-strong", rgbToHex(mixToward(rgb, 0, 0.42)));
    node.style.setProperty("--accent-quiet", `rgba(${rgb[0]}, ${rgb[1]}, ${rgb[2]}, 0.14)`);
  } else {
    // No/invalid accent: drop the whole set so it falls back to the .osf-ui
    // default and never leaks from the previously-selected mod.
    ACCENT_TOKENS.forEach((t) => node.style.removeProperty(t));
  }
}

function renderDetailHead(mod, schema, isFramework) {
  const head = el("div", "detail-head");
  const left = el("div");
  left.appendChild(el("div", "osf-eyebrow kicker", isFramework ? "Framework" : "Mod · " + mod.id));
  left.appendChild(el("h2", null, titleOf(mod)));
  if (schema.description) left.appendChild(el("div", "detail-desc", schema.description));
  head.appendChild(left);

  const reset = el("button", "osf-btn osf-btn--danger osf-btn--sm", "Reset all");
  reset.type = "button";
  reset.addEventListener("click", () => resetMod(mod.id));
  head.appendChild(reset);
  return head;
}

function renderPresets(mod, schema) {
  if (!Array.isArray(schema.presets) || !schema.presets.length) return null;
  const bar = el("div", "presets");
  bar.appendChild(el("span", "osf-eyebrow", "Presets"));
  const row = el("div", "presets-row");
  for (const p of schema.presets) {
    if (!p || typeof p.values !== "object") continue;
    const b = el("button", "osf-btn osf-btn--sm osf-btn--ghost", p.label || "Preset");
    b.type = "button";
    if (p.description) b.title = p.description;
    b.addEventListener("click", () => applyPreset(mod, p));
    row.appendChild(b);
  }
  bar.appendChild(row);
  return bar;
}

function applyPreset(mod, preset) {
  let n = 0;
  for (const key in preset.values) {
    applyLocal(mod.id, key, preset.values[key]);
    setValue(mod.id, key, preset.values[key]);
    n++;
  }
  // Rebuild the pane so every control (switch/slider/segmented/…) re-seeds from
  // the values just committed — refreshLive only updates conditions/dots, not
  // control state, and native doesn't re-broadcast settings.data on a set.
  // Preset buttons only ever act on the currently-displayed mod.
  renderDetail();
  toast(`Applied "${preset.label}" (${n} setting${n === 1 ? "" : "s"})`, "info");
}

// ---- surfaces (panels + HUD toggles) ----------------------------------------
// The catalog views attached to the selected entry, rendered as rows above the
// settings groups: a menu gets an Open button (menu.open — the opened panel
// replaces this surface on the single-menu stack), a HUD gets a switch
// (hud.show / hud.hide, optimistic flip reconciled by the views.data push).

function surfacePanelRow(v) {
  const row = el("div", "row");
  row.dataset.label = (v.title || "").toLowerCase();
  const text = el("div", "row-text");
  text.appendChild(el("div", "row-label", v.title || v.id));
  if (v.description) text.appendChild(el("div", "row-hint", v.description));
  row.appendChild(text);

  const control = el("div", "control");
  const failed = v.loadState === "failed";
  const btn = el("button", "osf-btn osf-btn--sm " + (failed ? "osf-btn--danger" : "osf-btn--accent"),
    failed ? "Failed" : "Open");
  btn.type = "button";
  if (failed) { btn.disabled = true; btn.title = "The view failed to load; see OSF UI.log."; }
  btn.addEventListener("click", () => {
    sendCommand({ command: "menu.open", view: v.id });
    // The opened panel replaces this surface; if it never comes up (failed
    // registration), don't leave the button stuck.
    btn.disabled = true; btn.textContent = "Opening…";
    setTimeout(() => { btn.disabled = false; btn.textContent = "Open"; }, 1600);
  });
  control.appendChild(btn);
  row.appendChild(control);
  surfaceControls.push({ viewId: v.id, kind: "menu", control: btn });
  return row;
}

function surfaceHudRow(v) {
  const row = el("div", "row");
  row.dataset.label = (v.title || "").toLowerCase();
  const text = el("div", "row-text");
  text.appendChild(el("div", "row-label", v.title || v.id));
  if (v.description) text.appendChild(el("div", "row-hint", v.description));
  row.appendChild(text);

  const control = el("div", "control");
  const sw = document.createElement("button");
  sw.type = "button"; sw.className = "osf-switch"; sw.setAttribute("role", "switch");
  const set = (on) => sw.setAttribute("aria-pressed", on ? "true" : "false");
  set(v.open === true);
  sw.addEventListener("click", () => {
    const next = sw.getAttribute("aria-pressed") !== "true";
    set(next);
    sendCommand({ command: next ? "hud.show" : "hud.hide", view: v.id });
  });
  control.appendChild(sw);
  row.appendChild(control);
  surfaceControls.push({ viewId: v.id, kind: "hud", control: sw });
  return row;
}

function renderSurfaces(views) {
  if (!views.length) return null;
  const menus = views.filter((v) => v.kind === "menu");
  const huds = views.filter((v) => v.kind !== "menu");
  const section = el("div", "group");
  const label = menus.length && huds.length ? "Terminals & overlays" : menus.length ? "Terminals" : "Overlays";
  const heading = el("button", "group-label", label);
  heading.type = "button";
  heading.addEventListener("click", () => section.classList.toggle("collapsed"));
  section.appendChild(heading);
  const rowsWrap = el("div", "group-rows");
  menus.forEach((v) => rowsWrap.appendChild(surfacePanelRow(v)));
  huds.forEach((v) => rowsWrap.appendChild(surfaceHudRow(v)));
  section.appendChild(rowsWrap);
  return section;
}

// Reconcile switch/button state against a fresh views.data without a teardown.
function refreshSurfaceStates() {
  for (const sc of surfaceControls) {
    const v = allViews.find((x) => x.id === sc.viewId);
    if (!v) continue;
    if (sc.kind === "hud") sc.control.setAttribute("aria-pressed", v.open ? "true" : "false");
  }
}

function renderRestartBanner(mod) {
  // Any changed-from-default setting flagged requires:"restart" pins a banner.
  const values = mod.values || {};
  let n = 0;
  for (const lr of liveRows) {
    if (lr.requires === "restart" && lr.setting && isModified(lr.setting, values[lr.setting.key])) n++;
  }
  if (!n) return null;
  const banner = el("div", "banner banner--warn");
  banner.appendChild(el("span", "banner-text",
    `${n} change${n === 1 ? "" : "s"} take effect after a game restart.`));
  return banner;
}

function renderDetail() {
  liveRows = []; liveGroups = []; surfaceControls = [];
  // The old buttons are about to be discarded; drop any pending action state so
  // a late ack can't restore a detached node.
  pendingActions.clear();
  detailEl.textContent = "";

  const q = (filterEl.value || "").trim().toLowerCase();
  if (q) { renderSearch(q); return; }

  if (selectedId === HOME_ID) { renderHomeDetail(); return; }

  const entry = findEntry(selectedId);
  if (!entry) {
    const empty = el("div", "detail-empty");
    empty.appendChild(el("div", "osf-eyebrow", "Nothing selected"));
    detailEl.appendChild(empty);
    return;
  }
  if (!entry.mod) { renderViewOnlyDetail(entry); return; }

  const mod = entry.mod;
  const isFramework = mod.id === FRAMEWORK_ID;
  const schema = mod.schema || {};
  const values = mod.values || {};
  applyAccent(detailEl, schema.accent);

  detailEl.appendChild(renderDetailHead(mod, schema, isFramework));

  const body = el("div", "detail-body");
  const surfaces = renderSurfaces(entry.views);
  if (surfaces) body.appendChild(surfaces);
  const presets = renderPresets(mod, schema);
  if (presets) body.appendChild(presets);

  const bannerSlot = el("div", "banner-slot");
  body.appendChild(bannerSlot);

  const groups = schema.groups || [];
  const autoIndex = groups.filter((g) => g.label).length > 4;
  if (autoIndex) body.appendChild(renderSectionIndex(groups));

  for (const group of groups) {
    const section = el("div", "group");
    if (group.collapsed) section.classList.add("collapsed");
    if (group.label) {
      const heading = el("button", "group-label", group.label);
      heading.type = "button";
      heading.addEventListener("click", () => section.classList.toggle("collapsed"));
      section.appendChild(heading);
      section.id = "grp-" + group.label.toLowerCase().replace(/\s+/g, "-");
    }
    const rowsWrap = el("div", "group-rows");
    for (const item of group.settings || []) {
      const row = buildItem(mod, item, values);
      if (row) rowsWrap.appendChild(row);
    }
    section.appendChild(rowsWrap);
    liveGroups.push({ el: section, visibleWhen: group.visibleWhen });
    body.appendChild(section);
  }
  detailEl.appendChild(body);

  const banner = renderRestartBanner(mod);
  if (banner) bannerSlot.appendChild(banner);

  refreshLive(mod);
}

// ---- Home (launcher) --------------------------------------------------------
// The landing page: every registered panel and overlay across all mods as an
// app-style card grid — the launch surface, where settings rows stay per-mod.
// Cards derive a monogram + accent from the view id so unbranded views still
// look intentional (same trick the old hub used). All untrusted text (titles,
// descriptions) goes through el()/textContent; the patch SVG is a static
// template tinted via currentColor.

const HOME_PALETTE = ["#6f93b0", "#7a9a5e", "#c98a4a", "#b96f86", "#8b83c0", "#b9a45e", "#5f9aa0", "#a8846a"];
function hashId(id) {
  let h = 0;
  for (let i = 0; i < id.length; i++) h = (h * 31 + id.charCodeAt(i)) >>> 0;
  return h;
}
function homeAccentFor(id) { return HOME_PALETTE[hashId(String(id)) % HOME_PALETTE.length]; }

// Owning-mod caption for a card: the settings mod's title, or the manifest
// `mod` string as-is for view-only mods.
function homeModCaption(v) {
  if (!v.mod) return "";
  const owner = allMods.find((m) => m.id === v.mod);
  return owner ? titleOf(owner) : v.mod;
}

function homePatch(accent, failed) {
  const wrap = el("span", "home-patch");
  wrap.style.color = failed ? "var(--signal-stop)" : accent;
  // Static markup only — no untrusted text passes through here.
  wrap.innerHTML = `<svg width="64" height="64" viewBox="0 0 200 200" aria-hidden="true">
    <circle cx="100" cy="100" r="93" fill="rgba(11,14,18,0.55)" stroke="currentColor" stroke-width="2" opacity="0.9"></circle>
    <circle cx="100" cy="100" r="83" fill="none" stroke="currentColor" stroke-width="1" opacity="0.32"></circle>
    <polygon points="22,100 27,94 32,100 27,106" fill="currentColor" opacity="0.8"></polygon>
    <polygon points="178,100 173,94 168,100 173,106" fill="currentColor" opacity="0.8"></polygon>
    ${failed ? '<g stroke="currentColor" stroke-width="6" stroke-linecap="round" fill="none"><path d="M100 78 v26"></path><path d="M100 118 v.5"></path></g>' : ""}
  </svg>`;
  return wrap;
}

function homeSectionHead(title, count, note) {
  const head = el("div", "home-head");
  head.appendChild(el("span", "home-head-title", title));
  head.appendChild(el("span", "home-head-count", String(count).padStart(2, "0")));
  head.appendChild(el("span", "home-head-rule"));
  head.appendChild(el("span", "home-head-note", note));
  return head;
}

function homeMenuCard(v) {
  const failed = v.loadState === "failed";
  const accent = homeAccentFor(v.id);
  const card = document.createElement("button");
  card.type = "button";
  card.className = "home-tile" + (failed ? " failed" : "");
  card.dataset.label = (v.title || "").toLowerCase();

  const patch = homePatch(accent, failed);
  if (!failed) {
    // Owning mod's icon inside the ring where the monogram sits; initials
    // when there is none (or the file is missing — same fallback as the rail).
    const mono = el("span", "home-monogram", initials(v.title || v.id));
    mono.style.color = accent;
    const src = ownerIconSrc(v.mod);
    if (src) {
      const img = document.createElement("img");
      img.className = "home-patch-icon";
      img.src = src;
      img.alt = "";
      img.addEventListener("error", () => { img.remove(); patch.appendChild(mono); });
      patch.appendChild(img);
    } else {
      patch.appendChild(mono);
    }
  }
  card.appendChild(patch);

  const body = el("span", "home-tile-body");
  body.appendChild(el("span", "home-tile-title", v.title || v.id));
  if (v.description) body.appendChild(el("span", "home-tile-desc", v.description));
  const caption = homeModCaption(v);
  if (caption) body.appendChild(el("span", "home-tile-mod", caption));
  card.appendChild(body);

  card.appendChild(el("span", "home-tile-foot", failed ? "FAILED — SEE OSF UI.LOG" : "OPEN ▸"));

  if (failed) {
    card.disabled = true;
  } else {
    card.addEventListener("click", () => {
      // Single-menu policy: the opened panel replaces this surface, so no
      // local state to reconcile — just guard against a dead double-click.
      sendCommand({ command: "menu.open", view: v.id });
      card.disabled = true;
      setTimeout(() => { card.disabled = false; }, 1600);
    });
  }
  return card;
}

function homeHudCard(v) {
  const accent = homeAccentFor(v.id);
  const card = document.createElement("button");
  card.type = "button";
  card.className = "home-hud";
  card.setAttribute("role", "switch");
  card.setAttribute("aria-pressed", v.open === true ? "true" : "false");

  const chip = el("span", "home-hud-chip");
  chip.style.color = accent;
  const chipSrc = ownerIconSrc(v.mod);
  const chipFallback = () => {
    chip.classList.remove("home-hud-chip--icon");
    chip.textContent = initials(v.title || v.id);
  };
  if (chipSrc) {
    chip.classList.add("home-hud-chip--icon");
    const img = document.createElement("img");
    img.src = chipSrc;
    img.alt = "";
    img.addEventListener("error", () => { img.remove(); chipFallback(); });
    chip.appendChild(img);
  } else {
    chipFallback();
  }
  card.appendChild(chip);

  const main = el("span", "home-hud-main");
  main.appendChild(el("span", "home-hud-name", v.title || v.id));
  const caption = homeModCaption(v);
  main.appendChild(el("span", "home-hud-desc", v.description || caption || ""));
  card.appendChild(main);

  const sw = el("span", "osf-switch home-hud-switch");
  card.appendChild(sw);

  card.addEventListener("click", () => {
    const next = card.getAttribute("aria-pressed") !== "true";
    card.setAttribute("aria-pressed", next ? "true" : "false");
    sendCommand({ command: next ? "hud.show" : "hud.hide", view: v.id });
  });
  // Registered as kind "hud" so refreshSurfaceStates reconciles aria-pressed
  // from views.data pushes exactly like the per-mod switch rows; the switch
  // visual keys off the card's aria-pressed in CSS.
  surfaceControls.push({ viewId: v.id, kind: "hud", control: card });
  return card;
}

function renderHomeDetail() {
  applyAccent(detailEl, null);
  const menus = allViews.filter((v) => v.kind === "menu");
  const huds = allViews.filter((v) => v.kind !== "menu");

  const head = el("div", "detail-head");
  const left = el("div");
  left.appendChild(el("div", "osf-eyebrow kicker", "All systems"));
  left.appendChild(el("h2", null, "Systems"));
  left.appendChild(el("div", "detail-desc",
    "Every terminal and overlay your mods have brought online. Select a mod on the left to reach its settings."));
  head.appendChild(left);
  detailEl.appendChild(head);

  const body = el("div", "detail-body detail-body--home");

  if (!allViews.length) {
    const empty = el("div", "home-empty");
    empty.appendChild(el("div", "osf-eyebrow", "No systems online"));
    empty.appendChild(el("p", null, "Mods that put terminals or overlays on screen appear here as launch cards. Mods that only carry settings are listed on the left."));
    body.appendChild(empty);
    detailEl.appendChild(body);
    return;
  }

  if (menus.length) {
    body.appendChild(homeSectionHead("Terminals", menus.length, "SELECT TO OPEN · DECK CLOSES"));
    const grid = el("div", "home-grid");
    menus.forEach((v) => grid.appendChild(homeMenuCard(v)));
    body.appendChild(grid);
  }
  if (huds.length) {
    body.appendChild(homeSectionHead("Overlays", huds.length, "TOGGLE · STAYS ON SCREEN"));
    const list = el("div", "home-hud-list");
    huds.forEach((v) => list.appendChild(homeHudCard(v)));
    body.appendChild(list);
  }
  detailEl.appendChild(body);
}

// A mod that registered views but no settings schema: same page shape, no
// settings groups and no Reset all.
function renderViewOnlyDetail(entry) {
  applyAccent(detailEl, null);
  const lead = entry.views.find((v) => v.kind === "menu") || entry.views[0];

  const head = el("div", "detail-head");
  const left = el("div");
  left.appendChild(el("div", "osf-eyebrow kicker", "Mod" + (lead.mod ? " · " + lead.mod : "")));
  left.appendChild(el("h2", null, entry.title));
  if (lead.description) left.appendChild(el("div", "detail-desc", lead.description));
  head.appendChild(left);
  detailEl.appendChild(head);

  const body = el("div", "detail-body");
  const surfaces = renderSurfaces(entry.views);
  if (surfaces) body.appendChild(surfaces);
  body.appendChild(el("p", "detail-quiet", "This mod registers no settings."));
  detailEl.appendChild(body);
}

function renderSectionIndex(groups) {
  const nav = el("div", "section-index");
  for (const g of groups) {
    if (!g.label) continue;
    const a = el("button", "section-index-item", g.label);
    a.type = "button";
    a.addEventListener("click", () => {
      const target = document.getElementById("grp-" + g.label.toLowerCase().replace(/\s+/g, "-"));
      if (target) { target.classList.remove("collapsed"); target.scrollIntoView({ block: "start" }); }
    });
    nav.appendChild(a);
  }
  return nav;
}

// Global search: a flat, cross-mod result list. Clicking jumps to that mod.
function renderSearch(q) {
  const head = el("div", "detail-head");
  const left = el("div");
  left.appendChild(el("div", "osf-eyebrow kicker", "Search"));
  left.appendChild(el("h2", null, `Results for "${q}"`));
  head.appendChild(left);
  detailEl.appendChild(head);

  const body = el("div", "detail-body");
  const list = el("div", "search-results");
  let hits = 0;
  for (const mod of allMods) {
    for (const g of (mod.schema && mod.schema.groups) || []) {
      for (const s of g.settings || []) {
        if (!isSetting(s) || typeof s.key !== "string" || !s.key) continue;
        const label = (s.label || s.key || "");
        if (!label.toLowerCase().includes(q) && !titleOf(mod).toLowerCase().includes(q)) continue;
        hits++;
        const item = el("button", "search-result"); item.type = "button";
        const crumb = el("div", "search-crumb");
        crumb.appendChild(el("span", "search-mod", titleOf(mod)));
        if (g.label) { crumb.appendChild(el("span", "search-sep", "›")); crumb.appendChild(el("span", null, g.label)); }
        item.appendChild(crumb);
        item.appendChild(el("div", "search-label", label));
        item.addEventListener("click", () => {
          filterEl.value = "";
          selectMod(mod.id);
          const t = document.querySelector(`.row[data-key="${cssEscape(s.key)}"]`);
          if (t) {
            const grp = t.closest(".group");
            if (grp) grp.classList.remove("collapsed");  // the target may sit in a collapsed group
            t.classList.add("flash"); t.scrollIntoView({ block: "center" });
            setTimeout(() => t.classList.remove("flash"), 1200);
          }
        });
        list.appendChild(item);
      }
    }
  }
  if (!hits) list.appendChild(el("div", "detail-empty", "No settings match."));
  body.appendChild(list);
  detailEl.appendChild(body);
}

function cssEscape(s) {
  if (window.CSS && CSS.escape) return CSS.escape(s);
  return String(s).replace(/["\\\]]/g, "\\$&");
}

// Re-evaluate conditions, modified dots, and the session chip without a
// teardown — toggles classes on the rows built by the last render.
function refreshLive(mod) {
  const values = mod.values || {};
  for (const lr of liveRows) {
    if (lr.visibleWhen) lr.row.classList.toggle("hidden-cond", !evalCondition(lr.visibleWhen, values));
    if (lr.enabledWhen && lr.control) {
      const on = evalCondition(lr.enabledWhen, values);
      lr.row.classList.toggle("disabled", !on);
      // Scope to the control itself (not the whole row) so the per-setting reset
      // button stays usable, and skip a mid-flight action button.
      const nodes = lr.control.matches && lr.control.matches("input,select,button,textarea")
        ? [lr.control] : lr.control.querySelectorAll("input,select,button,textarea");
      nodes.forEach((n) => { if (!n.classList.contains("pending")) n.disabled = !on; });
    }
    if (lr.dot && lr.setting) lr.dot.classList.toggle("on", isModified(lr.setting, values[lr.setting.key]));
    if (lr.setting) lr.row.classList.toggle("is-modified", isModified(lr.setting, values[lr.setting.key]));
  }
  for (const g of liveGroups) {
    if (g.visibleWhen) g.el.classList.toggle("hidden-cond", !evalCondition(g.visibleWhen, values));
  }
  const banner = renderRestartBanner(mod);
  const slot = detailEl.querySelector(".banner-slot");
  if (slot) { slot.textContent = ""; if (banner) slot.appendChild(banner); }
  updateChip();
  updateRailCounts();
}

function updateRailCounts() {
  for (const btn of railEl.querySelectorAll(".rail-item")) {
    const mod = allMods.find((m) => m.id === btn.dataset.mod);
    if (!mod) continue;
    let badge = btn.querySelector(".rail-item-count");
    const count = modifiedCount(mod);
    if (count && !badge) { badge = el("span", "rail-item-count"); btn.appendChild(badge); }
    if (badge) {
      if (count) {
        badge.textContent = String(count);
        badge.title = `${count} changed from default`;  // keep parity with railItem
        badge.style.display = "";
      } else { badge.style.display = "none"; }
    }
  }
}

// ---- undo chip + revert panel ----------------------------------------------
// Everything is saved automatically (write-behind — see the save-state line),
// so this must never read as "unsaved changes": it is an UNDO facility over
// what you touched since opening settings this time.

function updateChip() {
  if (!sessionChipEl) return;
  const n = sessionChangeCount();
  sessionChipEl.style.display = n ? "" : "none";
  sessionChipEl.textContent = `Undo changes (${n})`;
}

function openSessionPanel() {
  const changes = [];
  for (const modId in baseline) {
    const mod = allMods.find((m) => m.id === modId);
    if (!mod) continue;
    const values = mod.values || {};
    for (const key in baseline[modId]) {
      const old = baseline[modId][key];
      const cur = values[key];
      if (cur !== old) changes.push({ modId, key, old, now: cur, mod });
    }
  }
  if (!changes.length) return;

  const overlay = el("div", "session-overlay");
  const panel = el("div", "session-panel osf-card");
  const head = el("div", "session-head");
  head.appendChild(el("div", "osf-eyebrow", `Changed this visit (${changes.length})`));
  const revertAll = el("button", "osf-btn osf-btn--sm osf-btn--danger", "Revert all"); revertAll.type = "button";
  head.appendChild(revertAll);
  panel.appendChild(head);
  panel.appendChild(el("p", "session-note",
    "Everything is already saved. Revert anything you've changed since opening settings."));

  const list = el("div", "session-list");
  for (const c of changes) {
    const rowEl = el("div", "session-row");
    const info = el("div", "session-info");
    info.appendChild(el("div", "session-key", `${titleOf(c.mod)} · ${c.key}`));
    info.appendChild(el("div", "session-delta", `${JSON.stringify(c.old)} → ${JSON.stringify(c.now)}`));
    rowEl.appendChild(info);
    const undo = el("button", "osf-btn osf-btn--sm osf-btn--ghost", "Revert"); undo.type = "button";
    undo.addEventListener("click", () => { revertOne(c); close(); });
    rowEl.appendChild(undo);
    list.appendChild(rowEl);
  }
  panel.appendChild(list);

  const close = () => overlay.remove();
  overlay.addEventListener("click", (e) => { if (e.target === overlay) close(); });
  revertAll.addEventListener("click", () => { changes.forEach(revertOne); close(); });
  overlay.appendChild(panel);
  document.body.appendChild(overlay);
}

function revertOne(c) {
  applyLocal(c.modId, c.key, c.old);
  setValue(c.modId, c.key, c.old);
  if (c.modId === selectedId) { const mod = allMods.find((m) => m.id === c.modId); if (mod) renderDetail(); }
  else { updateChip(); updateRailCounts(); }
}

// ---- key rebind capture (one at a time) -----------------------------------

let capturing = null;  // { mod, key, btn, prev }

function beginCapture(mod, key, btn) {
  if (capturing) return;
  capturing = { mod, key, btn, prev: btn.textContent };
  btn.classList.add("listening");
  btn.textContent = "Press a key…";
  if (bridgeAvailable()) {
    sendCommand({ command: "settings.captureKey", mod, key });  // native captures the next key
    // Native arms capture for any key-typed setting and replies `cancelled` if
    // it declines. The timeout is a safety net (older runtime, dropped reply):
    // without it the button would stay on "Press a key…" AND `capturing` would
    // never clear, jamming every later rebind.
    capturing.timer = setTimeout(() => {
      if (capturing && capturing.btn === btn) {
        finishCapture({ mod, key, cancelled: true });
        toast("Rebinding didn't get a response from the runtime.", "warn");
      }
    }, ACTION_TIMEOUT_MS);
  } else {
    const onKey = (e) => {
      window.removeEventListener("keydown", onKey, true);
      e.preventDefault();
      const name = domKeyName(e);
      const cancelled = e.key === "Escape" || !name;
      finishCapture({ mod, key, name, cancelled,
        conflicts: cancelled ? [] : localKeyConflicts(name, mod, key) });
    };
    window.addEventListener("keydown", onKey, true);
  }
}

// Standalone-only preview of SettingsStore::ConflictsFor (the in-game path
// gets `conflicts` in settings.captured from native): the OTHER key settings
// whose current value is the captured name. Native groups by resolved VK; here
// a string compare is enough, like the harness mock.
function localKeyConflicts(name, mod, key) {
  const others = [];
  for (const m of allMods) {
    for (const g of (m.schema && m.schema.groups) || []) {
      for (const s of g.settings || []) {
        if (s && s.type === "key" && (m.values || {})[s.key] === name &&
            (m.id !== mod || s.key !== key)) {
          others.push({ mod: m.id, key: s.key, title: titleOf(m) });
        }
      }
    }
  }
  return others;
}

// Standalone-only: map a browser KeyboardEvent to an OSF UI key name (the
// in-game path does this natively via KeyName(vk)). Rough but enough to preview.
function domKeyName(e) {
  if (/^F([1-9]|1[0-9]|2[0-4])$/.test(e.key)) return e.key;
  if (/^[a-z]$/i.test(e.key)) return e.key.toUpperCase();
  if (/^[0-9]$/.test(e.key)) return e.key;
  const named = { " ": "Space", Enter: "Enter", Tab: "Tab", Backspace: "Backspace",
    Insert: "Insert", Delete: "Delete", Home: "Home", End: "End",
    PageUp: "PageUp", PageDown: "PageDown", ArrowUp: "Up", ArrowDown: "Down",
    ArrowLeft: "Left", ArrowRight: "Right", "`": "Grave" };
  return named[e.key] || "";
}

function finishCapture(payload) {
  if (!capturing) return;
  const { mod, key, btn, prev, timer } = capturing;
  if (timer) clearTimeout(timer);
  capturing = null;
  btn.classList.remove("listening");
  if (!payload || payload.cancelled || !payload.name) {
    btn.textContent = prev;
    return;
  }
  btn.textContent = payload.name;
  // Live-warn (mcm-design §9): the runtime checked the captured key against
  // every other key-typed binding BEFORE this commit lands, so the warning
  // shows now instead of after the settings.data round-trip (which is what
  // repaints the row badges). Informational — the bind stands.
  if (Array.isArray(payload.conflicts) && payload.conflicts.length) {
    const others = [...new Set(payload.conflicts.map((c) => c.title || c.mod))];
    toast(`${payload.name} is also bound by: ${others.join(", ")}`, "warn");
  }
  commit(mod, key, payload.name);  // persist + re-resolve, and update session state
}

// ---- selection + render ----------------------------------------------------

function selectMod(id) {
  selectedId = id;
  renderRail();
  renderDetail();
}

function captureBaseline() {
  for (const mod of allMods) {
    const b = baselineFor(mod.id);
    for (const key in (mod.values || {})) {
      if (!(key in b)) b[key] = mod.values[key];
    }
  }
}

function render() {
  const entries = railEntries();
  if (!entries.length) {
    railEl.textContent = "";
    detailEl.textContent = "";
    detailEl.appendChild(el("p", "status osf-eyebrow", "No mods installed — nothing has registered settings or views yet."));
    updateChip();
    return;
  }
  captureBaseline();
  if (pendingHashSelect &&
      (pendingHashSelect === HOME_ID || entries.some((e) => e.id === pendingHashSelect))) {
    selectedId = pendingHashSelect;
    pendingHashSelect = null;  // honored once; later pushes must not override clicks
  }
  if (selectedId !== HOME_ID && !entries.some((e) => e.id === selectedId)) {
    selectedId = HOME_ID;  // the launcher is the deck's landing page
  }
  renderRail();
  renderDetail();
  updateChip();
}

// Debounced: every keystroke would otherwise rebuild the rail AND run the
// cross-mod search scan + full pane teardown.
let filterTimer = 0;
filterEl.addEventListener("input", () => {
  clearTimeout(filterTimer);
  filterTimer = setTimeout(() => { renderRail(); renderDetail(); }, 120);
});
if (sessionChipEl) sessionChipEl.addEventListener("click", openSessionPanel);

// ---- native -> web ---------------------------------------------------------

function onNativeMessage(jsonText) {
  let message;
  try { message = JSON.parse(jsonText); } catch { return; }
  switch (message.type) {
    case "runtime.ready": {
      const version = message.payload && message.payload.version;
      const badge = document.getElementById("plugin-version");
      if (badge && version) badge.textContent = "v" + version;
      sendCommand({ command: "settings.get" });
      sendCommand({ command: "views.get" });  // also subscribes to change pushes
      break;
    }
    case "settings.data":
      allMods = (message.payload && message.payload.mods) || [];
      render();
      break;
    case "views.data": {
      allViews = ((message.payload && message.payload.views) || [])
        .filter((v) => v && v.hub !== false);
      // Pushes arrive on every open/focus flip (e.g. our own HUD toggle) —
      // only rebuild the pane when the catalog itself changed, or a settings
      // control mid-edit would be torn down under the user.
      const key = allViews.map((v) => [v.id, v.mod, v.title, v.kind, v.loadState].join("\u0000")).join("\n");
      if (key !== viewsShapeKey) {
        viewsShapeKey = key;
        render();
      } else {
        refreshSurfaceStates();
      }
      break;
    }
    case "settings.changed": {
      // Native push for every committed value — our own commits echo back
      // (possibly clamped), and other writers (a sibling DLL, a mod's panel, a
      // preset in another view) stay in sync while the menu is open.
      const p = message.payload || {};
      if (typeof p.mod !== "string" || typeof p.key !== "string") break;
      const mod = allMods.find((m) => m.id === p.mod);
      if (!mod) break;
      mod.values = mod.values || {};
      const b = baselineFor(p.mod);
      if (!(p.key in b)) b[p.key] = mod.values[p.key];
      // A key rebind can create or clear a cross-mod conflict, but `conflicts`
      // is only recomputed in settings.data — a plain settings.changed can't
      // carry the new state (and it may affect ANOTHER mod's badge). Pull a
      // fresh registry so every badge re-derives. Rare event; the full refresh
      // is fine. Handled before the echo check so our OWN rebind (already
      // applied optimistically) still triggers it.
      const changedSetting = findSettingInMod(mod, p.key);
      if (changedSetting && changedSetting.type === "key") {
        mod.values[p.key] = p.value;
        sendCommand({ command: "settings.get" });
        break;
      }
      if (mod.values[p.key] === p.value) {
        // The common case: the echo of our own optimistic commit. Cheap sync.
        updateChip(); updateRailCounts();
        break;
      }
      mod.values[p.key] = p.value;
      // The store disagrees with the local model (native clamp or an external
      // writer): rebuild the pane so the visible control shows the real value.
      if (p.mod === selectedId) renderDetail();
      updateChip(); updateRailCounts();
      break;
    }
    case "settings.captured":
      finishCapture(message.payload);
      break;
    case "settings.ack":
      if (message.payload && !message.payload.ok) {
        toast(`Rejected "${message.payload.mod}.${message.payload.key}"`, "danger");
        saveStateAbandon(message.payload.mod);
        // Native refused the value; pull authoritative state back.
        sendCommand({ command: "settings.get" });
      }
      break;
    case "settings.persisted":
      // The mod's values file WRITE landed (write-behind flush) — distinct
      // from settings.changed, which is the immediate in-memory commit.
      if (message.payload) saveStatePersisted(message.payload.mod);
      break;
    case "ui.visibility":
      // A fresh overlay visit (the runtime pushes this on the closed->open
      // edge): the undo scope is "since you opened settings", so drop the old
      // baseline — the chip disappears until something changes this visit.
      // Without this the view, which keeps running while hidden, accumulates
      // every change of the whole game session.
      if (message.payload && message.payload.visible) {
        baseline = {};
        updateChip();
        // Land every visit on the launcher, filter cleared — the toggle key
        // means "open the deck", not "resume where a past visit left off".
        if (selectedId !== HOME_ID || filterEl.value) {
          filterEl.value = "";
          selectMod(HOME_ID);
        }
      }
      break;
    default:
      // Mod action acknowledgements: "<modId>.ack" with { key, ok, message }.
      // Resolve by mod+key (the mod id is the message-type prefix) so two mods
      // sharing an action key can't cross-resolve each other's buttons.
      if (typeof message.type === "string" && message.type.endsWith(".ack")) {
        const p = message.payload || {};
        const modId = message.type.slice(0, -".ack".length);
        const restore = p.key != null && pendingActions.get(modId + " " + p.key);
        if (restore) restore(p.message || (p.ok === false ? "Action failed" : null), p.ok === false ? "danger" : "info");
      }
      break;
  }
}

window.osfui = window.osfui || {};
window.osfui.onMessage = onNativeMessage;

document.getElementById("close").addEventListener("click", () => sendCommand({ command: "close" }));
document.addEventListener("keydown", (e) => {
  if ((e.ctrlKey || e.metaKey) && String(e.key).toLowerCase() === "f") {
    e.preventDefault();
    filterEl.focus();
    filterEl.select();
    return;
  }
  if (e.key === "Escape" && !e.defaultPrevented && !capturing && !document.querySelector(".session-overlay")) {
    sendCommand({ command: "close" });
  }
});

if (bridgeAvailable()) {
  sendCommand({ command: "settings.get" });
  sendCommand({ command: "views.get" });
} else {
  // Standalone (plain browser) — sample schemas + catalog views exercising the
  // widgets so the layout can be iterated without launching the game.
  allMods = sampleMods();
  allViews = sampleViews();
  render();
}

function sampleViews() {
  return [
    { id: "keybinds", title: "Keybinds", description: "Every key binding at a glance — mods, the game, and collisions. Rebind in place.", mod: "osfui", kind: "menu", hub: true, open: false, focused: false, loadState: "loaded" },
    { id: "almanac", title: "Ship Almanac", description: "Browse ship modules, mass and performance readouts.", mod: "demo", kind: "menu", hub: true, open: false, focused: false, loadState: "loaded" },
    { id: "atlas", title: "Star Atlas", description: "Annotated survey routes and anomalies by system.", mod: "", kind: "menu", hub: true, open: false, focused: false, loadState: "failed" },
    { id: "hudwidgets", title: "HUD Widgets", description: "Clock and status overlays over the live game.", mod: "demo", kind: "hud", hub: true, open: true, loadState: "loaded" },
  ];
}

function sampleMods() {
  return [
    {
      id: "osfui", title: "OSF UI",
      schema: {
        description: "Runtime and overlay behavior for the OSF UI framework itself.",
        groups: [
          { label: "Input", settings: [
            { key: "toggleKey", label: "Open / close key", type: "key", default: "F10", hint: "Rebind the overlay key." },
          ] },
          { label: "Overlay", settings: [
            { key: "allowPanels", label: "Allow mod settings panels", type: "bool", default: true, hint: "Custom mod panels run in this view's context.", requires: "reload" },
          ] },
        ],
      },
      values: { toggleKey: "F10", allowPanels: true },
    },
  ];
}
