// Keybinds view — every key binding at a glance, rebind in place.
//
// A visual keyboard map (mod-bound keys glow accent, game-bound keys steel,
// collisions warn), a holders panel for the selected key, and a searchable
// list of every binding. Data is the same `settings.data` document the
// settings view consumes: every `type:"key"` setting of every mod, plus the
// top-level `vanillaKeys` table (the game's own bindings — read-only rows).
// Rebinds reuse the generic capture machinery (`settings.captureKey` →
// `settings.captured` → echoed `settings.set`), including the capture-time
// conflict live-warn. `ui.hotkey` pushes flash the pressed key on the board.
//
// Grouping is by KEY NAME with the same alias folding as native
// (Tilde/Backtick/Console → Grave, Return → Enter), so the board agrees with
// the store's vk-resolved conflict data without re-resolving VKs in JS.

"use strict";

const ACTION_TIMEOUT_MS = 5000;

const boardEl = document.getElementById("keyboard");
const detailEl = document.getElementById("detail");
const detailTitleEl = document.getElementById("detail-title");
const listEl = document.getElementById("bindlist");
const listTitleEl = document.getElementById("list-title");
const searchEl = document.getElementById("search");
const statusEl = document.getElementById("status");
const toastEl = document.getElementById("toast");

let mods = [];        // settings.data mods (id, title, schema, values)
let vanilla = [];     // settings.data vanillaKeys [{event, title, name}]
let bindings = [];    // flattened rows, mod + game (see buildModel)
let selectedKey = ""; // canonical key name ("" = nothing selected)
let capturing = null; // { mod, key, btn, prev, timer }

// ---- bridge -----------------------------------------------------------------

function bridgeAvailable() {
  return typeof window.osfui === "object" &&
         typeof window.osfui.postMessage === "function";
}
function sendCommand(fields) {
  if (bridgeAvailable()) {
    window.osfui.postMessage(JSON.stringify({ type: "ui.command", payload: fields }));
  }
}

// ---- utils --------------------------------------------------------------------

function el(tag, className, text) {
  const n = document.createElement(tag);
  if (className) n.className = className;
  if (text != null) n.textContent = text;
  return n;
}

function toast(message, kind) {
  if (!toastEl) return;
  const t = el("div", "toast" + (kind ? " toast--" + kind : ""), message);
  toastEl.appendChild(t);
  setTimeout(() => { t.classList.add("leaving"); }, 2600);
  setTimeout(() => { t.remove(); }, 3000);
}

// Fold key-name aliases to the canonical KeyName() spelling so string
// grouping agrees with native's vk-resolved grouping.
const NAME_ALIASES = { tilde: "Grave", backtick: "Grave", console: "Grave", return: "Enter" };
function canonicalName(name) {
  const s = String(name || "");
  const folded = NAME_ALIASES[s.toLowerCase()];
  if (folded) return folded;
  if (/^[a-z]$/.test(s)) return s.toUpperCase();  // letters store uppercase
  return s;
}

// "Starfield (Quicksave)" -> "Quicksave" for display inside a GAME-tagged row.
function vanillaLabel(title) {
  const m = /^Starfield \((.+)\)$/.exec(String(title || ""));
  return m ? m[1] : String(title || "");
}

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

// ---- model --------------------------------------------------------------------

function buildModel() {
  bindings = [];
  for (const mod of mods) {
    for (const g of (mod.schema && mod.schema.groups) || []) {
      for (const s of g.settings || []) {
        if (!s || s.type !== "key" || typeof s.key !== "string") continue;
        const value = (mod.values || {})[s.key];
        if (typeof value !== "string" || !value) continue;
        const context = inputContextFor(mod, s);
        bindings.push({
          kind: "mod",
          mod: mod.id,
          key: s.key,
          label: s.label || s.key,
          owner: mod.title || mod.id,
          name: canonicalName(value),
          contextId: context.id,
          contextLabel: context.label,
          blocksGameplay: context.blocksGameplay,
        });
      }
    }
  }
  for (const v of vanilla) {
    bindings.push({
      kind: "game",
      key: v.event,
      label: vanillaLabel(v.title),
      owner: "Starfield",
      name: canonicalName(v.name),
      contextId: "gameplay",
      contextLabel: "Gameplay",
      blocksGameplay: false,
    });
  }
}

function holdersOf(name) {
  return bindings.filter((b) => b.name === name);
}

function pairIsShared(a, b) {
  const mod = a.kind === "mod" && b.kind === "game" ? a
    : b.kind === "mod" && a.kind === "game" ? b : null;
  return !!(mod && mod.blocksGameplay);
}

function keyState(name) {
  const holders = holdersOf(name);
  let conflict = false;
  let shared = false;
  for (let i = 0; i < holders.length; ++i) {
    for (let j = i + 1; j < holders.length; ++j) {
      if (pairIsShared(holders[i], holders[j])) shared = true;
      else conflict = true;
    }
  }
  return { conflict, shared };
}

function holderState(binding) {
  let conflict = false;
  let shared = false;
  for (const other of holdersOf(binding.name)) {
    if (other === binding) continue;
    if (pairIsShared(binding, other)) shared = true;
    else conflict = true;
  }
  return { conflict, shared };
}

// ---- keyboard layout ------------------------------------------------------------
// d = printed label; n = OSF UI key name (null = not bindable by mods: either
// the name doesn't resolve natively, or — Esc — capture treats it as cancel).
// w = width units; "gap" entries are spacers.

const K = (d, n, w) => ({ d, n: n === undefined ? d : n, w: w || 1 });
const DEAD = (d, w) => ({ d, n: null, w: w || 1 });
const GAP = (w) => ({ gap: w });

const KEYBOARD_MAIN = [
  [DEAD("Esc", 1.2), GAP(0.55), K("F1"), K("F2"), K("F3"), K("F4"), GAP(0.35), K("F5"), K("F6"), K("F7"), K("F8"), GAP(0.35), K("F9"), K("F10"), K("F11"), K("F12")],
  [K("`", "Grave"), K("1"), K("2"), K("3"), K("4"), K("5"), K("6"), K("7"), K("8"), K("9"), K("0"), DEAD("-"), DEAD("="), K("Bksp", "Backspace", 1.9)],
  [K("Tab", "Tab", 1.45), K("Q"), K("W"), K("E"), K("R"), K("T"), K("Y"), K("U"), K("I"), K("O"), K("P"), DEAD("["), DEAD("]"), DEAD("\\", 1.45)],
  [K("Caps", "CapsLock", 1.75), K("A"), K("S"), K("D"), K("F"), K("G"), K("H"), K("J"), K("K"), K("L"), DEAD(";"), DEAD("'"), K("Enter", "Enter", 2.15)],
  [K("Shift", "LShift", 2.25), K("Z"), K("X"), K("C"), K("V"), K("B"), K("N"), K("M"), DEAD(","), DEAD("."), DEAD("/"), K("Shift", "RShift", 2.65)],
  [K("Ctrl", "LCtrl", 1.4), K("Alt", "LAlt", 1.4), K("Space", "Space", 7.3), K("Alt", "RAlt", 1.4), K("Ctrl", "RCtrl", 1.4)],
];
const KEYBOARD_NAV = [
  [K("Ins", "Insert"), K("Home"), K("PgUp", "PageUp")],
  [K("Del", "Delete"), K("End"), K("PgDn", "PageDown")],
  [GAP(3)],
  [GAP(1), K("↑", "Up"), GAP(1)],
  [K("←", "Left"), K("↓", "Down"), K("→", "Right")],
];

const keyCells = new Map();  // canonical name -> cell element

function renderKeyboard() {
  boardEl.textContent = "";
  keyCells.clear();
  const renderRows = (rows) => {
    const block = el("div", "kb-block");
    for (const row of rows) {
      const rowEl = el("div", "kb-row");
      for (const item of row) {
        if ("gap" in item) {
          const g = el("span", "kb-gap");
          g.style.flexGrow = String(item.gap);
          rowEl.appendChild(g);
          continue;
        }
        const cell = el("button", "kb-key");
        cell.type = "button";
        cell.style.flexGrow = String(item.w);
        cell.style.flexBasis = 0;
        cell.appendChild(el("span", "kb-key-label", item.d));
        if (item.n) {
          cell.dataset.name = item.n;
          cell.appendChild(el("span", "kb-key-holders"));
          cell.addEventListener("click", () => selectKey(item.n));
          keyCells.set(item.n, cell);
        } else {
          cell.classList.add("is-dead");
          cell.disabled = true;
          cell.title = item.d === "Esc" ? "Reserved (cancels rebinds)" : "Not bindable by mods";
        }
        rowEl.appendChild(cell);
      }
      block.appendChild(rowEl);
    }
    return block;
  };
  boardEl.appendChild(renderRows(KEYBOARD_MAIN));
  boardEl.appendChild(renderRows(KEYBOARD_NAV));
  paintKeyboard();
}

function paintKeyboard() {
  const q = searchEl.value.trim().toLowerCase();
  for (const [name, cell] of keyCells) {
    const holders = holdersOf(name);
    const hasMod = holders.some((b) => b.kind === "mod");
    const hasGame = holders.some((b) => b.kind === "game");
    const state = keyState(name);
    cell.classList.toggle("is-mod", hasMod && holders.length === 1);
    cell.classList.toggle("is-game", hasGame && !hasMod && holders.length === 1);
    cell.classList.toggle("is-shared", state.shared && !state.conflict);
    cell.classList.toggle("is-conflict", state.conflict);
    cell.classList.toggle("is-selected", name === selectedKey);
    cell.classList.toggle("is-dim", !!q && !holders.some(matchesQuery(q)) && !name.toLowerCase().includes(q));
    const dots = cell.querySelector(".kb-key-holders");
    if (dots) {
      dots.textContent = "";
      for (const b of holders.slice(0, 3)) {
        dots.appendChild(el("i", "kb-dot kb-dot--" + b.kind));
      }
    }
    const who = holders.map((b) => `${b.owner}: ${b.label}`).join("\n");
    cell.title = who || name;
  }
}

// ---- search ---------------------------------------------------------------------

function matchesQuery(q) {
  return (b) => !q ||
    b.name.toLowerCase().includes(q) ||
    b.label.toLowerCase().includes(q) ||
    b.owner.toLowerCase().includes(q);
}

// ---- detail panel ----------------------------------------------------------------

function selectKey(name) {
  selectedKey = name === selectedKey ? "" : name;
  renderDetail();
  paintKeyboard();
}

function holderRow(b) {
  const row = el("div", "kb-holder");
  const text = el("div", "kb-holder-text");
  const line = el("div", "kb-holder-title");
  line.appendChild(el("span", null, b.label));
  line.appendChild(el("span", "osf-badge " + (b.kind === "game" ? "osf-badge--ghost" : "osf-badge--accent"),
    b.kind === "game" ? "GAME" : b.owner));
  if (b.kind === "mod" && b.contextId !== "gameplay") {
    line.appendChild(el("span", "osf-badge kb-context", b.contextLabel));
  }
  text.appendChild(line);
  const identity = b.kind === "game" ? `controlmap · ${b.key}` : `${b.mod}.${b.key}`;
  text.appendChild(el("div", "kb-holder-sub", `${identity} · ${b.contextLabel}`));
  row.appendChild(text);
  const keyChip = el("span", "kb-chip", b.name);
  row.appendChild(keyChip);
  if (b.kind === "mod") {
    const btn = el("button", "osf-btn osf-btn--sm osf-key", "Rebind");
    btn.type = "button";
    btn.addEventListener("click", () => beginCapture(b.mod, b.key, btn));
    row.appendChild(btn);
  }
  return row;
}

function renderDetail() {
  detailEl.textContent = "";
  if (!selectedKey) {
    detailTitleEl.textContent = "Select a key";
    detailEl.appendChild(el("p", "kb-hint", "Click any key on the board to see what holds it."));
    return;
  }
  const holders = holdersOf(selectedKey);
  detailTitleEl.textContent = "";
  detailTitleEl.appendChild(el("span", "kb-chip kb-chip--lg", selectedKey));
  detailTitleEl.appendChild(document.createTextNode(
    holders.length ? ` ${holders.length} binding${holders.length > 1 ? "s" : ""}` : " unbound"));
  const state = keyState(selectedKey);
  if (state.conflict) {
    detailTitleEl.appendChild(el("span", "osf-badge osf-badge--stop", "Key conflict"));
  }
  if (state.shared) {
    detailTitleEl.appendChild(el("span", "osf-badge kb-shared-badge", "Shared across contexts"));
  }
  if (!holders.length) {
    detailEl.appendChild(el("p", "kb-hint", "Nothing is bound here."));
    return;
  }
  for (const b of holders) {
    detailEl.appendChild(holderRow(b));
  }
}

// ---- all-bindings list -------------------------------------------------------------

// F2 before F10 before Insert: F-keys numerically first, then the rest alpha.
function keyOrder(name) {
  const f = /^F(\d+)$/.exec(name);
  return f ? `0${String(parseInt(f[1], 10)).padStart(3, "0")}` : `1${name}`;
}

function renderList() {
  const q = searchEl.value.trim().toLowerCase();
  const rows = bindings.filter(matchesQuery(q))
    .sort((a, b) => keyOrder(a.name).localeCompare(keyOrder(b.name)) ||
      a.owner.localeCompare(b.owner));
  listTitleEl.textContent = `All bindings (${rows.length})`;
  listEl.textContent = "";
  for (const b of rows) {
    const row = holderRow(b);
    row.classList.add("kb-holder--list");
    const state = holderState(b);
    if (state.conflict) row.classList.add("kb-holder--conflict");
    else if (state.shared) row.classList.add("kb-holder--shared");
    row.addEventListener("click", (e) => {
      if (e.target.closest("button")) return;  // Rebind clicks stay rebinds
      selectKey(b.name);
    });
    listEl.appendChild(row);
  }
  if (!rows.length) {
    listEl.appendChild(el("p", "kb-hint", q ? "No bindings match." : "No key bindings registered."));
  }
}

function renderAll() {
  buildModel();
  renderKeyboard();
  renderDetail();
  renderList();
  statusEl.style.display = "none";
}

// ---- rebind capture (one at a time) ------------------------------------------------

function beginCapture(mod, key, btn) {
  if (capturing) return;
  capturing = { mod, key, btn, prev: btn.textContent };
  btn.classList.add("listening");
  btn.textContent = "Press a key…";
  if (bridgeAvailable()) {
    sendCommand({ command: "settings.captureKey", mod, key });
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
      finishCapture({ mod, key, name, cancelled: e.key === "Escape" || !name });
    };
    window.addEventListener("keydown", onKey, true);
  }
}

// Standalone-only preview mapping (the in-game path names keys natively).
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
  btn.textContent = prev;
  if (!payload || payload.cancelled || !payload.name) {
    return;
  }
  // Live-warn (mcm-design §9): the runtime already checked the captured key
  // against every other binding — surface it now, before the commit lands.
  if (Array.isArray(payload.conflicts) && payload.conflicts.length) {
    const others = [...new Set(payload.conflicts.map((c) => c.title || c.mod))];
    toast(`${payload.name} is also bound by: ${others.join(", ")}`, "warn");
  }
  // Optimistic local apply + the authoritative echo (settings.set).
  const mod2 = mods.find((m) => m.id === mod);
  if (mod2) {
    mod2.values = mod2.values || {};
    mod2.values[key] = payload.name;
  }
  sendCommand({ command: "settings.set", mod, key, value: payload.name });
  selectedKey = canonicalName(payload.name);
  renderAll();
}

// ---- hotkey flash ------------------------------------------------------------------

function flashKey(mod, key) {
  const b = bindings.find((x) => x.kind === "mod" && x.mod === mod && x.key === key);
  const cell = b && keyCells.get(b.name);
  if (!cell) return;
  cell.classList.remove("is-flash");
  void cell.offsetWidth;  // restart the animation
  cell.classList.add("is-flash");
}

// ---- messages ----------------------------------------------------------------------

function onMessage(json) {
  let message;
  try { message = JSON.parse(json); } catch { return; }
  if (!message || typeof message.type !== "string") return;
  const p = message.payload || {};
  switch (message.type) {
    case "runtime.ready":
      sendCommand({ command: "settings.get" });
      break;
    case "settings.data":
      mods = Array.isArray(p.mods) ? p.mods : [];
      vanilla = Array.isArray(p.vanillaKeys) ? p.vanillaKeys : [];
      renderAll();
      break;
    case "settings.changed": {
      // Only key-typed settings matter here (the schema says which); update
      // the local value and repaint. Non-key traffic is ignored.
      const mod = mods.find((m) => m.id === p.mod);
      const isKey = mod && ((mod.schema && mod.schema.groups) || []).some((g) =>
        (g.settings || []).some((s) => s && s.key === p.key && s.type === "key"));
      if (isKey && typeof p.value === "string") {
        mod.values = mod.values || {};
        mod.values[p.key] = p.value;
        renderAll();
      }
      break;
    }
    case "settings.captured":
      finishCapture(p);
      break;
    case "ui.hotkey":
      flashKey(p.mod, p.key);
      break;
    default:
      break;
  }
}

// ---- boot --------------------------------------------------------------------------

searchEl.addEventListener("input", () => { paintKeyboard(); renderList(); });
document.getElementById("close").addEventListener("click", () => sendCommand({ command: "close" }));
document.addEventListener("keydown", (e) => {
  if ((e.ctrlKey || e.metaKey) && String(e.key).toLowerCase() === "f") {
    e.preventDefault();
    searchEl.focus();
    searchEl.select();
    return;
  }
  if (e.key === "Escape" && !e.defaultPrevented && !capturing) {
    sendCommand({ command: "close" });
  }
});

if (bridgeAvailable()) {
  window.osfui.onMessage = onMessage;
  sendCommand({ command: "settings.get" });
} else {
  // Standalone preview (plain browser, no harness): sample data.
  mods = [
    { id: "osfui", title: "OSF UI", values: { toggleKey: "F10" },
      schema: { groups: [{ settings: [{ key: "toggleKey", label: "Open / close key", type: "key", default: "F10" }] }] } },
  ];
  vanilla = [
    { event: "QuickSave", title: "Starfield (Quicksave)", name: "F5" },
    { event: "QuickLoad", title: "Starfield (Quickload)", name: "F9" },
    { event: "Activate", title: "Starfield (Interact)", name: "E" },
    { event: "Jump", title: "Starfield (Jump)", name: "Space" },
    { event: "Console", title: "Starfield (Console)", name: "Grave" },
  ];
  renderAll();
}
