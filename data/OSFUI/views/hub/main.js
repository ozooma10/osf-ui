// OSF UI — Hub / view launcher.
//
// Pure bridge consumer (bridge protocol 0.2). On runtime.ready it sends
// `views.get`, which both replies with the surface catalog AND subscribes this
// view to change pushes — so the hub reflects open/focus/load state live with
// no polling. It launches menus with `menu.open` (single-menu policy: the
// opened menu replaces the hub, so the hub closes) and toggles HUDs with
// `hud.show` / `hud.hide`. Everything it does is a whitelisted command; there
// is no hub-specific native API.

"use strict";

// ---- bridge plumbing --------------------------------------------------------

function bridgeAvailable() {
  return typeof window.osfui === "object" &&
         typeof window.osfui.postMessage === "function";
}

function sendCommand(fields) {
  if (bridgeAvailable()) {
    window.osfui.postMessage(JSON.stringify({ type: "ui.command", payload: fields }));
  }
}

// ---- DOM handles ------------------------------------------------------------

const el = {
  statusWrap:   document.querySelector(".status"),
  statusLabel:  document.getElementById("status-label"),
  statusCounts: document.getElementById("status-counts"),
  menuGrid:     document.getElementById("menu-grid"),
  hudList:      document.getElementById("hud-list"),
  menusSection: document.getElementById("menus-section"),
  hudsSection:  document.getElementById("huds-section"),
  menusCount:   document.getElementById("menus-count"),
  hudsCount:    document.getElementById("huds-count"),
  hudsActive:   document.getElementById("huds-active"),
  emptyNote:    document.getElementById("empty-note"),
  hints:        document.getElementById("hints"),
  version:      document.getElementById("footer-version"),
  launch:       document.getElementById("launch-overlay"),
  launchTitle:  document.getElementById("launch-title"),
};

// ---- monogram / accent derivation ------------------------------------------
// No mod is required to ship art: an accent + monogram is derived from the id
// so every tile looks intentional. (Named icons would need the runtime to
// serve per-view assets across the sandbox — deferred; see the roadmap.)

const PALETTE = ["#6f93b0", "#7a9a5e", "#c98a4a", "#b96f86", "#8b83c0", "#b9a45e", "#5f9aa0", "#a8846a"];

function hashId(id) {
  let h = 0;
  for (let i = 0; i < id.length; i++) h = (h * 31 + id.charCodeAt(i)) >>> 0;
  return h;
}
function accentFor(id) { return PALETTE[hashId(id) % PALETTE.length]; }
function rgba(hex, a) {
  const n = parseInt(hex.slice(1), 16);
  return `rgba(${(n >> 16) & 255}, ${(n >> 8) & 255}, ${n & 255}, ${a})`;
}
function initialsFor(title) {
  const w = title.trim().split(/\s+/);
  if (w.length >= 2) return (w[0][0] + w[1][0]).toUpperCase();
  return w[0].replace(/[^A-Za-z0-9]/g, "").slice(0, 2).toUpperCase();
}

// The "mission patch" emblem drawn behind every menu monogram, tinted to the
// tile accent via currentColor. Kept as a string template so tiles are cheap.
function patchSvg(failed) {
  const fault = failed
    ? '<g stroke="currentColor" stroke-width="6" stroke-linecap="round" fill="none"><path d="M100 78 v26"></path><path d="M100 118 v.5"></path></g>'
    : "";
  return `<svg width="78" height="78" viewBox="0 0 200 200">
    <circle cx="100" cy="100" r="93" fill="rgba(11,14,18,0.55)" stroke="currentColor" stroke-width="2" opacity="0.9"></circle>
    <circle cx="100" cy="100" r="83" fill="none" stroke="currentColor" stroke-width="1" opacity="0.32"></circle>
    <g stroke="currentColor" stroke-width="1" opacity="0.4" fill="none"><line x1="52" y1="60" x2="80" y2="50"></line><line x1="120" y1="52" x2="150" y2="66"></line><line x1="150" y1="140" x2="120" y2="150"></line></g>
    <g fill="currentColor" opacity="0.7"><circle cx="52" cy="60" r="1.6"></circle><circle cx="80" cy="50" r="1.2"></circle><circle cx="150" cy="66" r="1.6"></circle><circle cx="120" cy="150" r="1.3"></circle><circle cx="150" cy="140" r="1.2"></circle></g>
    <polygon points="22,100 27,94 32,100 27,106" fill="currentColor" opacity="0.8"></polygon>
    <polygon points="178,100 173,94 168,100 173,106" fill="currentColor" opacity="0.8"></polygon>
    ${fault}
  </svg>`;
}

// ---- rendering --------------------------------------------------------------

let currentViews = [];

function menuTile(v) {
  const failed = v.loadState === "failed";
  const focused = !!v.focused && !failed;
  const acc = accentFor(v.id);

  const tile = document.createElement("button");
  tile.type = "button";
  tile.className = "menu-tile" + (focused ? " focused" : "") + (failed ? " failed" : "");
  tile.dataset.nav = "1";

  // state pill
  let pillClass = "", pillLabel = "READY", lit = false;
  if (failed)      { pillClass = "stop lit";  pillLabel = "FAILED";  lit = true; }
  else if (focused) { pillClass = "focus lit"; pillLabel = "FOCUSED"; lit = true; }
  else if (v.open)  { pillClass = "go lit";    pillLabel = "OPEN";    lit = true; }

  const patchColor = failed ? "var(--signal-stop)" : acc;

  tile.innerHTML = `
    <span class="corner tl"></span><span class="corner tr"></span>
    <span class="corner bl"></span><span class="corner br"></span>
    <span class="tile-top">
      <span class="kind-tag">MENU</span>
      <span class="state-pill ${pillClass}"><span class="dot"></span>${pillLabel}</span>
    </span>
    <span class="patch" style="color:${patchColor};">
      ${patchSvg(failed)}
      ${failed ? "" : `<span class="monogram">${initialsFor(v.title)}</span>`}
    </span>
    <span class="tile-body">
      <span class="tile-title">${escapeHtml(v.title)}</span>
      <span class="tile-desc">${escapeHtml(v.description || "")}</span>
    </span>
    <span class="tile-foot">${failed ? "RELOADING" : "OPEN"} ${failed ? "" : chevron()}</span>`;

  if (!failed) {
    tile.addEventListener("click", () => openMenu(v));
  }
  return tile;
}

function chevron() {
  return `<svg width="13" height="13" viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="2.4" stroke-linecap="round" stroke-linejoin="round"><polyline points="9 6 15 12 9 18"></polyline></svg>`;
}

function hudRow(v) {
  const on = !!v.open;
  const acc = accentFor(v.id);

  const row = document.createElement("button");
  row.type = "button";
  row.className = "hud-row" + (on ? " on" : "");
  row.dataset.nav = "1";
  row.innerHTML = `
    <span class="hud-rail"></span>
    <span class="hud-chip" style="${on ? `color:${acc}; background:${rgba(acc, 0.12)}; border-color:${rgba(acc, 0.5)};` : ""}">${initialsFor(v.title)}</span>
    <span class="hud-main">
      <span class="hud-name-row">
        <span class="hud-name">${escapeHtml(v.title)}</span>
      </span>
      <span class="hud-desc">${escapeHtml(v.description || "")}</span>
    </span>
    <span class="hud-side">
      <span class="hud-track"><span class="hud-knob"></span></span>
      <span class="hud-state">${on ? "● LIVE" : "○ HIDDEN"}</span>
    </span>`;

  row.addEventListener("click", () => {
    // Optimistic flip for responsiveness; the views.data push reconciles it.
    sendCommand({ command: on ? "hud.hide" : "hud.show", view: v.id });
    row.classList.toggle("on");
  });
  return row;
}

function render(views) {
  currentViews = views;
  const shown = views.filter((v) => v.hub !== false);
  const menus = shown.filter((v) => v.kind === "menu");
  const huds = shown.filter((v) => v.kind === "hud");
  const activeHuds = huds.filter((h) => h.open).length;

  el.menuGrid.replaceChildren(...menus.map(menuTile));
  el.hudList.replaceChildren(...huds.map(hudRow));

  el.menusSection.hidden = menus.length === 0;
  el.hudsSection.hidden = huds.length === 0;
  el.emptyNote.hidden = shown.length !== 0;

  el.menusCount.textContent = pad(menus.length);
  el.hudsCount.textContent = pad(huds.length);
  el.hudsActive.textContent = pad(activeHuds);
  el.statusCounts.textContent = `· ${pad(shown.length)} VIEWS · ${pad(activeHuds)} LIVE`;
}

function pad(n) { return String(n).padStart(2, "0"); }

function escapeHtml(s) {
  return String(s).replace(/[&<>"']/g, (c) =>
    ({ "&": "&amp;", "<": "&lt;", ">": "&gt;", '"': "&quot;", "'": "&#39;" }[c]));
}

// ---- actions ----------------------------------------------------------------

function openMenu(v) {
  el.launchTitle.textContent = v.title;
  el.launch.hidden = false;
  sendCommand({ command: "menu.open", view: v.id });
  // The hub is replaced on the single-menu stack and hides; if it somehow
  // stays up (e.g. target failed to register), don't leave the flourish stuck.
  clearTimeout(openMenu._t);
  openMenu._t = setTimeout(() => { el.launch.hidden = true; }, 1600);
}

// ---- keyboard / gamepad roving navigation ----------------------------------
// The engine maps D-pad/left-stick to arrow keys, so arrow-driven roving focus
// makes the hub controller-navigable. Enter/Space activate the focused tile
// (native button behavior). Esc/F10 are handled by the runtime (close overlay).

function navItems() {
  return Array.from(document.querySelectorAll('[data-nav="1"]'));
}
document.addEventListener("keydown", (e) => {
  if (!["ArrowUp", "ArrowDown", "ArrowLeft", "ArrowRight"].includes(e.key)) return;
  const items = navItems();
  if (items.length === 0) return;
  const idx = items.indexOf(document.activeElement);
  let next;
  if (idx === -1) {
    next = 0;
  } else if (e.key === "ArrowDown" || e.key === "ArrowRight") {
    next = (idx + 1) % items.length;
  } else {
    next = (idx - 1 + items.length) % items.length;
  }
  items[next].focus();
  e.preventDefault();
});

// ---- native -> web ----------------------------------------------------------

function markConnected(ready) {
  el.statusWrap.classList.toggle("ok", ready);
  el.statusLabel.textContent = ready ? "RUNTIME NOMINAL" : "CONNECTING";
}

function onNativeMessage(jsonText) {
  let message;
  try { message = JSON.parse(jsonText); } catch { return; }
  switch (message.type) {
    case "runtime.ready":
      markConnected(true);
      if (message.payload && message.payload.version) {
        el.version.textContent = "v" + message.payload.version;
      }
      sendCommand({ command: "views.get" });
      break;
    case "views.data":
      // A fresh push means a menu we launched (or a HUD toggle) landed — clear
      // any lingering launch flourish.
      el.launch.hidden = true;
      render((message.payload && message.payload.views) || []);
      break;
    default:
      break;
  }
}

window.osfui = window.osfui || {};
window.osfui.onMessage = onNativeMessage;

// ---- footer hints -----------------------------------------------------------

const HINTS = [
  { key: "↑↓", label: "Navigate" },
  { key: "↵", label: "Open" },
  { key: "TAB", label: "Cycle" },
  { key: "ESC", label: "Close" },
  { key: "F10", label: "Close" },
];
el.hints.replaceChildren(...HINTS.map((h) => {
  const w = document.createElement("span");
  w.className = "hint";
  w.innerHTML = `<span class="hint-key">${h.key}</span><span class="hint-label">${h.label}</span>`;
  return w;
}));

// ---- boot -------------------------------------------------------------------

if (bridgeAvailable()) {
  markConnected(false);
  sendCommand({ command: "views.get" });  // in case runtime.ready already fired
} else {
  // Standalone (plain browser) — render sample data so the layout can be
  // iterated without launching the game.
  markConnected(true);
  el.statusLabel.textContent = "STANDALONE";
  render([
    { id: "settings", title: "Settings", description: "Configure installed mods, hotkeys and runtime options.", kind: "menu", interactive: true, hub: true, open: false, focused: false, loadState: "loaded" },
    { id: "almanac", title: "Ship Almanac", description: "Browse ship modules, mass and performance readouts.", kind: "menu", interactive: true, hub: true, open: false, focused: true, loadState: "loaded" },
    { id: "cargo", title: "Cargo Manifest", description: "Sortable inventory with a live mass budget.", kind: "menu", interactive: true, hub: true, open: false, focused: false, loadState: "loaded" },
    { id: "atlas", title: "Star Atlas", description: "Annotated survey routes and anomalies by system.", kind: "menu", interactive: true, hub: true, open: false, focused: false, loadState: "failed" },
    { id: "hud", title: "HUD Widgets", description: "Clock and status overlays over the live game.", kind: "hud", interactive: false, hub: true, open: true, loadState: "loaded" },
    { id: "vitals", title: "Vitals Ring", description: "O2, health and affliction indicators.", kind: "hud", interactive: false, hub: true, open: false, loadState: "loaded" },
  ]);
}
