// Galactic Almanac — reference view logic.
//
// What this demonstrates for view authors:
//   1. The narrow JSON bridge + the runtime.ready handshake (with bridgeVersion
//      gating), and a clean standalone fallback so the page is iterable in a
//      plain browser.
//   2. A data-dense UI the native menus can't do well: instant text search,
//      faceted filtering, multi-key sort, master->detail, and cross-linking
//      (click a resource to find every body that has it).
//   3. Reading THIS view's own settings (settings/almanac.json) back out of the
//      shared registry and applying them CLIENT-SIDE — no core change. (Native
//      *reactions* to a new mod's settings still need a runtime edit; reading
//      your persisted values to drive your own DOM does not.)
//
// Everything native goes through window.starfield; see docs/authoring-views.md.

"use strict";

// ---------- bridge plumbing ----------

const statusEl = document.getElementById("status");

function bridgeAvailable() {
  return typeof window.starfield === "object" &&
         typeof window.starfield.postMessage === "function";
}

function sendCommand(fields) {
  if (bridgeAvailable()) {
    window.starfield.postMessage(JSON.stringify({ type: "ui.command", payload: fields }));
  }
}

// ---------- state ----------

const DATA = (window.ALMANAC && window.ALMANAC.bodies) || [];
const RES_NAMES = (window.ALMANAC && window.ALMANAC.resourceNames) || {};

const state = {
  search: "",
  type: "",
  landable: false,
  habitable: false,
  sort: "name",
  resource: "",          // active resource cross-link filter ("" = none)
  selectedId: null,
  compact: false,
};

const bodyId = (b) => `${b.system}/${b.name}`;

// ---------- filtering + sorting ----------

function matches(b) {
  if (state.type && b.type !== state.type) return false;
  if (state.landable && !b.landable) return false;
  if (state.habitable && !b.habitable) return false;
  if (state.resource && !b.resources.includes(state.resource)) return false;
  if (state.search) {
    const q = state.search.toLowerCase();
    const hay = [
      b.name, b.system, b.type,
      ...b.resources, ...b.resources.map((r) => RES_NAMES[r] || ""),
      ...(b.traits || []), ...(b.poi || []),
    ].join(" ").toLowerCase();
    if (!hay.includes(q)) return false;
  }
  return true;
}

function sortKey(b) {
  switch (state.sort) {
    case "gravity": return b.gravity;
    case "tempC": return b.tempC;
    case "resources": return b.resources.length;
    case "system": return `${b.system}/${b.name}`;
    default: return b.name;
  }
}

function visibleBodies() {
  const numeric = state.sort === "gravity" || state.sort === "tempC" || state.sort === "resources";
  const rows = DATA.filter(matches);
  rows.sort((a, b) => {
    const ka = sortKey(a), kb = sortKey(b);
    if (numeric) return kb - ka;             // numeric: high to low
    return String(ka).localeCompare(String(kb));
  });
  return rows;
}

// ---------- rendering: master list ----------

const listEl = document.getElementById("list");
const countEl = document.getElementById("count");
const activeResEl = document.getElementById("activeResource");

function renderList() {
  const rows = visibleBodies();
  listEl.textContent = "";

  for (const b of rows) {
    const li = document.createElement("li");
    li.className = "list-item" + (bodyId(b) === state.selectedId ? " selected" : "");
    li.dataset.id = bodyId(b);

    const name = document.createElement("div");
    name.className = "name";
    name.textContent = b.name;

    const stat = document.createElement("div");
    stat.className = "stat";
    stat.textContent = statForSort(b);

    const sub = document.createElement("div");
    sub.className = "sub";
    sub.textContent = `${b.system} · ${b.type}${b.parent ? ` of ${b.parent}` : ""}`;

    const badges = document.createElement("div");
    badges.className = "badges";
    if (b.landable) badges.appendChild(makeBadge("Landable", false));
    if (b.habitable) badges.appendChild(makeBadge("Habitable", true));
    if (b.fauna > 0) badges.appendChild(makeBadge(`${b.fauna} fauna`, false));
    if (b.flora > 0) badges.appendChild(makeBadge(`${b.flora} flora`, false));

    li.append(name, stat, sub, badges);
    li.addEventListener("click", () => select(bodyId(b)));
    listEl.appendChild(li);
  }

  countEl.textContent = `${rows.length} of ${DATA.length} bodies`;
  if (state.resource) {
    activeResEl.hidden = false;
    activeResEl.textContent = `${RES_NAMES[state.resource] || state.resource} ✕`;
  } else {
    activeResEl.hidden = true;
  }
}

function statForSort(b) {
  switch (state.sort) {
    case "gravity": return `${b.gravity.toFixed(2)} g`;
    case "tempC": return `${b.tempC}°C`;
    case "resources": return `${b.resources.length} res`;
    default: return `${b.gravity.toFixed(2)} g`;
  }
}

function makeBadge(text, good) {
  const el = document.createElement("span");
  el.className = "badge" + (good ? " good" : "");
  el.textContent = text;
  return el;
}

// ---------- rendering: detail ----------

const detailEl = document.getElementById("detail");

function select(id) {
  state.selectedId = id;
  renderList();
  renderDetail();
}

function renderDetail() {
  const b = DATA.find((x) => bodyId(x) === state.selectedId);
  if (!b) {
    detailEl.innerHTML = '<p class="empty">Select a body to view its survey data.</p>';
    return;
  }

  detailEl.textContent = "";

  const head = document.createElement("div");
  head.className = "detail-head";
  const h2 = document.createElement("h2");
  h2.textContent = b.name;
  const where = document.createElement("div");
  where.className = "where";
  where.textContent = `${b.system} system · ${b.type}${b.parent ? ` (moon of ${b.parent})` : ""}`;
  head.append(h2, where);

  const body = document.createElement("div");
  body.className = "detail-body";

  const grid = document.createElement("div");
  grid.className = "stat-grid";
  const stats = [
    ["Gravity", `${b.gravity.toFixed(2)} g`],
    ["Temperature", `${b.tempClass} · ${b.tempC}°C`],
    ["Atmosphere", b.atmosphere],
    ["Magnetosphere", b.magnetosphere],
    ["Surface water", `${b.water}%`],
    ["Flora", b.flora > 0 ? `${b.flora} species` : "None"],
    ["Fauna", b.fauna > 0 ? `${b.fauna} species` : "None"],
    ["Landable", b.landable ? "Yes" : "No"],
    ["Habitable", b.habitable ? "Yes" : "No"],
  ];
  for (const [k, v] of stats) {
    const cell = document.createElement("div");
    cell.className = "stat-cell";
    cell.innerHTML = `<div class="k"></div><div class="v"></div>`;
    cell.querySelector(".k").textContent = k;
    cell.querySelector(".v").textContent = v;
    grid.appendChild(cell);
  }
  body.appendChild(grid);

  if (b.resources.length) {
    body.appendChild(sectionLabel("Resources — click to find other bodies"));
    const chips = document.createElement("div");
    chips.className = "chips";
    for (const r of b.resources) {
      const chip = document.createElement("span");
      chip.className = "chip resource";
      chip.textContent = r;
      chip.title = RES_NAMES[r] || r;
      // Cross-link: filter the master list to every body with this resource.
      chip.addEventListener("click", () => {
        state.resource = r;
        renderList();
      });
      chips.appendChild(chip);
    }
    body.appendChild(chips);
  }

  if ((b.traits || []).length) {
    body.appendChild(sectionLabel("Traits"));
    body.appendChild(chipRow(b.traits, "trait"));
  }
  if ((b.poi || []).length) {
    body.appendChild(sectionLabel("Notable locations"));
    body.appendChild(chipRow(b.poi, "poi"));
  }

  detailEl.append(head, body);
}

function sectionLabel(text) {
  const el = document.createElement("div");
  el.className = "section-label";
  el.textContent = text;
  return el;
}

function chipRow(items, cls) {
  const row = document.createElement("div");
  row.className = "chips";
  for (const it of items) {
    const c = document.createElement("span");
    c.className = `chip ${cls}`;
    c.textContent = it;
    row.appendChild(c);
  }
  return row;
}

// ---------- control wiring ----------

document.getElementById("search").addEventListener("input", (e) => {
  state.search = e.target.value.trim();
  renderList();
});
document.getElementById("filterType").addEventListener("change", (e) => {
  state.type = e.target.value; renderList();
});
document.getElementById("filterLandable").addEventListener("change", (e) => {
  state.landable = e.target.checked; renderList();
});
document.getElementById("filterHabitable").addEventListener("change", (e) => {
  state.habitable = e.target.checked; renderList();
});
const sortEl = document.getElementById("sort");
sortEl.addEventListener("change", (e) => { state.sort = e.target.value; renderList(); });
activeResEl.addEventListener("click", () => { state.resource = ""; renderList(); });
document.getElementById("close").addEventListener("click", () => sendCommand({ command: "close" }));

// Software pointer follows routed mouse moves (OS cursor hidden in-game).
const cursorEl = document.getElementById("cursor");
document.addEventListener("mousemove", (e) => {
  if (cursorEl) { cursorEl.style.left = `${e.clientX}px`; cursorEl.style.top = `${e.clientY}px`; }
});

// ---------- settings: read our own persisted values and apply ----------

function applyOwnSettings(mods) {
  const mine = (mods || []).find((m) => m.id === "almanac");
  if (!mine) return;
  const v = mine.values || {};
  if (typeof v.defaultSort === "string") {
    state.sort = v.defaultSort;
    sortEl.value = v.defaultSort;
  }
  if (typeof v.onlyLandable === "boolean" && v.onlyLandable) {
    state.landable = true;
    document.getElementById("filterLandable").checked = true;
  }
  if (typeof v.compact === "boolean") {
    state.compact = v.compact;
    document.querySelector(".app").classList.toggle("compact", v.compact);
  }
  renderList();
}

// ---------- native -> web ----------

function onNativeMessage(jsonText) {
  let message;
  try { message = JSON.parse(jsonText); } catch { return; }
  switch (message.type) {
    case "runtime.ready": {
      const v = message.payload.bridgeVersion || "0.0";
      if (!v.startsWith("0.")) {
        statusEl.textContent = `Unsupported bridge ${v} — view may misbehave.`;
      } else {
        statusEl.textContent = `Connected · ${DATA.length} bodies`;
      }
      // Pull the shared settings registry so we can apply our own knobs.
      sendCommand({ command: "settings.get" });
      break;
    }
    case "settings.data":
      applyOwnSettings(message.payload.mods);
      break;
    default:
      break;   // unknown types are ignored, never eval'd
  }
}

window.starfield = window.starfield || {};
window.starfield.onMessage = onNativeMessage;

// ---------- boot ----------

renderList();
if (DATA.length) select(bodyId(DATA[0]));

if (bridgeAvailable()) {
  statusEl.textContent = "Connecting…";
  sendCommand({ command: "settings.get" });   // works even before runtime.ready; reply also handled above
} else {
  statusEl.textContent = `Standalone · ${DATA.length} bodies (no native bridge)`;
}
