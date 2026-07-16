// MockBridge — a browser stand-in for the OSF UI native bridge.
//
// Load this BEFORE the real settings view's main.js. It defines window.osfui so
// the view takes its normal bridge path (settings.get/set/reset/captureKey),
// and answers with the same validation/clamp rules as
// src/runtime/SettingsStore.cpp — so what you see here is what the game does.
// Values persist to localStorage; every message is logged to the console.
//
// Sources of schemas, in order: any ?schema=<url> query param, the repo's
// shipped data/OSFUI/settings/*.json (fetched when served over http), and a
// built-in fallback pair so the page works even from file://. Drop .json files
// onto the page to add/replace mods live.

"use strict";

(function () {
  const LS_PREFIX = "osfui.mock.";
  const MAX_KEY_NAME = 16;

  let mods = [];   // [{ id, title, schema, values }]
  const log = (dir, msg) => console.log(`%c[mock ${dir}]`, "color:#5aa9b8", msg);

  // ---- validation mirror (SettingsStore.cpp) ----
  function clampNumber(setting, v) {
    let n = Number(v);
    if (typeof setting.min === "number") n = Math.max(n, setting.min);
    if (typeof setting.max === "number") n = Math.min(n, setting.max);
    return setting.type === "int" ? Math.round(n) : n;
  }
  function validate(setting, value) {
    switch (setting.type) {
      case "bool": return typeof value === "boolean" ? value : undefined;
      case "int":
      case "float": return typeof value === "number" ? clampNumber(setting, value) : undefined;
      case "enum": return (setting.options || []).includes(value) ? value : undefined;
      case "string": {
        if (typeof value !== "string") return undefined;
        // A colour-widget string must be a parseable #rrggbb[aa], like native.
        if (setting.widget === "color" && !/^#([0-9a-fA-F]{6}|[0-9a-fA-F]{8})$/.test(value)) return undefined;
        // Native hard-caps at 256 today; the native slice raises this up to
        // 4096. Keep this in lockstep with the store.
        const cap = Math.min(256, setting.maxLength || 256);
        return value.length > cap ? value.slice(0, cap) : value;
      }
      case "key": {
        if (typeof value !== "string") return undefined;
        // "" = deliberately unbound — accepted only when the schema opts in,
        // like the store (allowUnbound).
        if (!value) return setting.allowUnbound ? value : undefined;
        return value.length > MAX_KEY_NAME ? value.slice(0, MAX_KEY_NAME) : value;
      }
      default: return undefined; // unknown type — refused, like the store
    }
  }
  function defaultFor(setting) { return "default" in setting ? setting.default : null; }

  function isSetting(item) {
    return item && ["bool", "int", "float", "enum", "string", "key"].includes(item.type);
  }
  function eachSetting(schema, fn) {
    for (const g of (schema && schema.groups) || []) {
      for (const s of g.settings || []) { if (isSetting(s)) fn(s); }
    }
  }

  const INPUT_CONTEXT_ID_RE = /^[A-Za-z0-9][A-Za-z0-9._-]{0,63}$/;
  function blocksGameplay(schema, setting) {
    const ref = setting && typeof setting.inputContext === "string" ? setting.inputContext : "";
    if (!ref || ref === "gameplay" || !INPUT_CONTEXT_ID_RE.test(ref)) return false;
    const seen = new Set();
    const contexts = schema && Array.isArray(schema.inputContexts) ? schema.inputContexts : [];
    for (const context of contexts) {
      if (!context || typeof context !== "object") continue;
      const id = typeof context.id === "string" ? context.id : "";
      if (id === "gameplay" || !INPUT_CONTEXT_ID_RE.test(id) || seen.has(id)) continue;
      seen.add(id);
      if (id === ref) return context.blocksGameplay === true;
    }
    return false;
  }

  function findSetting(mod, key) {
    let found = null;
    eachSetting(mod && mod.schema, (s) => { if (s.key === key) found = s; });
    return found;
  }

  // ---- persistence ----
  function loadSaved(id) {
    try { return JSON.parse(localStorage.getItem(LS_PREFIX + id) || "{}"); } catch { return {}; }
  }
  function persist(mod) {
    try { localStorage.setItem(LS_PREFIX + mod.id, JSON.stringify(mod.values)); } catch { /* ignore */ }
  }

  function buildMod(schema) {
    const id = schema.id || "mod";
    const saved = loadSaved(id);
    const values = {};
    eachSetting(schema, (s) => {
      const v = s.key in saved ? validate(s, saved[s.key]) : undefined;
      values[s.key] = v !== undefined ? v : defaultFor(s);
    });
    return { id, title: schema.title || id, schema, values };
  }

  // Mirror of SettingsStore id sanitization: ids become filenames and asset
  // path segments, so the store rejects unsafe/reserved ones.
  function validModId(id) {
    return typeof id === "string" && /^[A-Za-z0-9_-][A-Za-z0-9._-]{0,63}$/.test(id) && !id.includes("..") &&
           !["ui", "menu", "hud", "settings", "views", "game", "runtime"].includes(id);
  }

  function upsert(schema) {
    const mod = buildMod(schema);
    if (!validModId(mod.id)) { log("info", `rejected schema id "${mod.id}" (unsafe or reserved — the store refuses it too)`); return; }
    const i = mods.findIndex((m) => m.id === mod.id);
    if (i >= 0) mods[i] = mod; else mods.push(mod);
  }

  // ---- native -> web ----
  function send(type, payload) {
    log("→web", type);
    if (window.osfui && typeof window.osfui.onMessage === "function") {
      window.osfui.onMessage(JSON.stringify({ type, payload }));
    }
  }
  // The game's own bindings (mcm-design §9 "vanilla hotkeys"): native loads
  // vanillakeys.json + the engine's controlmap overrides and injects "@game"
  // pseudo-entries; the mock ships a small sample so the harness exercises
  // the "Starfield (…)" side of badges and capture live-warns.
  const VANILLA = [
    { name: "F5", event: "QuickSave", title: "Starfield (Quicksave)" },
    { name: "F9", event: "QuickLoad", title: "Starfield (Quickload)" },
    { name: "E", event: "Activate", title: "Starfield (Interact)" },
    { name: "Space", event: "Jump", title: "Starfield (Jump)" },
    { name: "Grave", event: "Console", title: "Starfield (Console)" },
  ];

  // Mirror SettingsStore::Data()'s key-conflict grouping (mcm-design §9): a
  // key setting whose bound value is also bound elsewhere gets
  // conflicts:[{mod,key,title}]. Native groups by RESOLVED vk; the mock groups
  // by the value string (close enough for harness visuals). Recomputed fresh
  // each send so a rebind that clears a conflict drops the badge.
  function annotateConflicts() {
    const byVal = {};
    for (const v of VANILLA) {
      (byVal[v.name] = byVal[v.name] || []).push({ mod: "@game", key: v.event, title: v.title });
    }
    for (const m of mods) {
      eachSetting(m.schema, (s) => { if (s.type === "key") delete s.conflicts; });
      eachSetting(m.schema, (s) => {
        if (s.type !== "key") return;
        const v = m.values[s.key];
        if (!v) return;
        (byVal[v] = byVal[v] || []).push({ mod: m.id, key: s.key, title: m.title });
      });
    }
    for (const m of mods) {
      eachSetting(m.schema, (s) => {
        if (s.type !== "key") return;
        const v = m.values[s.key];
        if (!v) return;
        const expectedGameReuse = blocksGameplay(m.schema, s);
        const others = (byVal[v] || []).filter((x) =>
          (x.mod !== m.id || x.key !== s.key) && !(expectedGameReuse && x.mod === "@game"));
        if (others.length) s.conflicts = others;
      });
    }
  }
  function sendData() {
    annotateConflicts();
    // Mirror SettingsStore::Data()'s top-level vanillaKeys table (the game's
    // own bindings, full map — the keybinds view renders it).
    send("settings.data", { mods,
      vanillaKeys: VANILLA.map((v) => ({ event: v.event, title: v.title, name: v.name })) });
  }

  // ---- view catalog (panels + HUDs on the Mods surface) ----
  // Mirrors the runtime's views.data push. The real shipped views come first —
  // menu.open on one that has a harness page navigates there, so panel launch
  // works inside the harness. The `fixture: true` entries are fictional: they
  // exercise every state the Mods surface renders (a view owned by a settings
  // mod — `mod` matches a schema id, view-only mods with no schema, a failed
  // load, HUD live / hidden). Hidden by default; toggle with the top-bar
  // "Sample views" button or ?fixtures=1 (persisted in localStorage).
  const HARNESS_PAGES = { settings: "index.html", keybinds: "keybinds.html", osf: "osf.html" };
  const views = [
    { id: "settings", title: "Mods", description: "Installed mods — settings, panels and HUD toggles.", mod: "osfui", kind: "menu", interactive: true, hub: false, open: false, focused: false, loadState: "loaded" },
    { id: "keybinds", title: "Keybinds", description: "Full keyboard map of mod and game bindings.", mod: "osfui", kind: "menu", interactive: true, hub: true, open: false, focused: false, loadState: "loaded" },
    // Real view from the sibling repo (VFS-merged in game). mod "osf" groups
    // it onto the OSF Animation settings page (schema registered natively —
    // see tryFetchNativeSchema). Open lands on osf.html.
    { id: "osf", title: "OSF Animation Browser", description: "Scene browser and launcher — crew, furniture, launch.", mod: "osf", kind: "menu", interactive: true, hub: true, open: false, focused: false, loadState: "loaded" },
    { id: "almanac", title: "Ship Almanac", description: "Browse ship modules, mass and performance readouts.", mod: "demo", kind: "menu", interactive: true, hub: true, open: false, focused: true, loadState: "loaded", fixture: true },
    { id: "hudwidgets", title: "HUD Widgets", description: "Clock and status overlays over the live game.", mod: "demo", kind: "hud", interactive: false, hub: true, open: true, loadState: "loaded", fixture: true },
    { id: "cargo", title: "Cargo Manifest", description: "Sortable inventory with a live mass budget.", mod: "", kind: "menu", interactive: true, hub: true, open: false, focused: false, loadState: "loaded", fixture: true },
    { id: "atlas", title: "Star Atlas", description: "Annotated survey routes and anomalies by system.", mod: "", kind: "menu", interactive: true, hub: true, open: false, focused: false, loadState: "failed", fixture: true },
    { id: "vitals", title: "Vitals Ring", description: "O2, health and affliction indicators.", mod: "", kind: "hud", interactive: false, hub: true, open: false, loadState: "loaded", fixture: true },
  ];
  const FIXTURES_LS = LS_PREFIX + "fixtures";
  const fixturesParam = new URLSearchParams(location.search).get("fixtures");
  if (fixturesParam !== null) {
    try { localStorage.setItem(FIXTURES_LS, fixturesParam === "1" ? "1" : "0"); } catch { /* ignore */ }
  }
  let fixturesOn = (() => { try { return localStorage.getItem(FIXTURES_LS) === "1"; } catch { return false; } })();
  function setFixtures(on) {
    fixturesOn = on === undefined ? !fixturesOn : !!on;
    try { localStorage.setItem(FIXTURES_LS, fixturesOn ? "1" : "0"); } catch { /* ignore */ }
    sendViews();
    return fixturesOn;
  }
  let readySent = false;
  function sendViews() { send("views.data", { views: views.filter((v) => fixturesOn || !v.fixture) }); }

  // Mirrors SettingsModule subscribe-on-read (protocol 0.3): settings.get
  // subscribes the page; committed values then push as settings.changed.
  let subscribed = false;
  function pushChanged(mod, key, value) {
    if (subscribed) setTimeout(() => send("settings.changed", { mod, key, value }), 0);
  }

  // Mirrors the native write-behind (SettingsStore::PumpPersistence, ~500ms
  // per-mod window opened at the first unflushed change): one
  // settings.persisted push per window confirms the "disk write". The mock's
  // localStorage persist() above is immediate — only the notification is
  // delayed, which is all the view can observe anyway.
  const persistTimers = new Map(); // modId -> timeout id
  function pushPersisted(modId) {
    if (persistTimers.has(modId)) return; // window already open — coalesce
    persistTimers.set(modId, setTimeout(() => {
      persistTimers.delete(modId);
      if (subscribed) send("settings.persisted", { mod: modId });
    }, 500));
  }

  // ---- web -> native ----
  function handle(p) {
    const cmd = p && p.command;
    switch (cmd) {
      case "settings.get":
        subscribed = true;
        setTimeout(sendData, 0);
        break;
      case "settings.set": {
        const mod = mods.find((m) => m.id === p.mod);
        let ok = false;
        if (mod) {
          let setting = null;
          eachSetting(mod.schema, (s) => { if (s.key === p.key) setting = s; });
          if (setting) {
            const v = validate(setting, p.value);
            if (v !== undefined) {
              mod.values[p.key] = v;
              persist(mod);
              ok = true;
              pushChanged(p.mod, p.key, v); // post-validation value, like native
              pushPersisted(p.mod);
            }
          }
        }
        setTimeout(() => send("settings.ack", { mod: p.mod, key: p.key, ok }), 0);
        break;
      }
      case "settings.reset": {
        const mod = mods.find((m) => m.id === p.mod);
        if (mod) {
          eachSetting(mod.schema, (s) => {
            if (!p.key || s.key === p.key) {
              mod.values[s.key] = defaultFor(s);
              pushChanged(p.mod, s.key, mod.values[s.key]);
            }
          });
          persist(mod);
          pushPersisted(p.mod);
          setTimeout(sendData, 0); // mirrors SettingsModule: re-send registry
        }
        break;
      }
      case "settings.captureKey": {
        // Captures ANY (mod,key), matching the in-game runtime: native arms
        // capture for every setting a schema declares `type:"key"`.
        const onKey = (e) => {
          window.removeEventListener("keydown", onKey, true);
          e.preventDefault();
          const name = domKeyName(e);
          const cancelled = e.key === "Escape" || !name;
          const payload = { mod: p.mod, key: p.key, name, cancelled };
          // Live-warn during capture (mcm-design §9): the OTHER key settings
          // already on the captured key, delivered BEFORE the view commits —
          // mirrors SettingsStore::ConflictsFor. Value-string compare, like
          // annotateConflicts(); omitted when unique, like native.
          if (!cancelled) {
            const targetMod = mods.find((m) => m.id === p.mod);
            const targetSetting = findSetting(targetMod, p.key);
            const expectedGameReuse = blocksGameplay(targetMod && targetMod.schema, targetSetting);
            const others = VANILLA.filter((v) => v.name === name && !expectedGameReuse)
              .map((v) => ({ mod: "@game", key: v.event, title: v.title }));
            for (const m of mods) {
              eachSetting(m.schema, (s) => {
                if (s.type === "key" && m.values[s.key] === name &&
                    (m.id !== p.mod || s.key !== p.key)) {
                  others.push({ mod: m.id, key: s.key, title: m.title });
                }
              });
            }
            if (others.length) payload.conflicts = others;
          }
          send("settings.captured", payload);
        };
        window.addEventListener("keydown", onKey, true);
        break;
      }
      case "views.get":
        // Reply with the catalog; first call also gets runtime.ready (once —
        // the Mods view answers ready with another views.get, so re-sending loops).
        setTimeout(() => {
          if (!readySent) { readySent = true; send("runtime.ready", { version: "0.2.0-mock" }); }
          sendViews();
        }, 0);
        break;
      case "menu.open": {
        const page = HARNESS_PAGES[p.view];
        if (page) {
          // Real shipped view — hand off to its harness page (brief delay,
          // like the in-game single-menu swap).
          log("info", `menu.open ${p.view} → ${page}`);
          setTimeout(() => { location.href = page; }, 450);
        } else {
          // Fictional view — just mark it open/focused and push, which clears
          // the launch overlay (mirrors the runtime's reconcile push).
          setTimeout(() => {
            for (const v of views) { if (v.kind === "menu") { v.focused = v.id === p.view; v.open = v.open || v.id === p.view; } }
            sendViews();
          }, 400);
        }
        break;
      }
      case "hud.show":
      case "hud.hide": {
        const v = views.find((x) => x.id === p.view);
        if (v) v.open = cmd === "hud.show";
        setTimeout(sendViews, 150); // async reconcile, like native
        break;
      }
      case "close":
        log("→native", "close (no-op in harness)");
        break;
      default:
        // Mod action command: reply with a simulated "<mod>.ack" so the button
        // resolves. Real mods do this from their own SFSE plugin.
        if (typeof cmd === "string" && cmd.includes(".") && !cmd.startsWith("ui.") &&
            !cmd.startsWith("settings.") && !cmd.startsWith("menu.") && !cmd.startsWith("hud.") &&
            !cmd.startsWith("views.")) {
          const modId = cmd.slice(0, cmd.indexOf("."));
          setTimeout(() => send(modId + ".ack", { key: p.key, ok: true, message: "Done (mock)" }), 400);
        }
        break;
    }
  }

  function domKeyName(e) {
    if (/^F([1-9]|1[0-9]|2[0-4])$/.test(e.key)) return e.key;
    if (/^[a-z]$/i.test(e.key)) return e.key.toUpperCase();
    if (/^[0-9]$/.test(e.key)) return e.key;
    const named = { " ": "Space", Enter: "Enter", Tab: "Tab", ArrowUp: "Up", ArrowDown: "Down",
      ArrowLeft: "Left", ArrowRight: "Right", "`": "Grave" };
    return named[e.key] || "";
  }

  // ---- schema sources ----
  const FALLBACK = [
    { id: "osfui", title: "OSF UI", description: "Framework runtime + overlay behavior.",
      groups: [
        { label: "Input", settings: [{ key: "toggleKey", label: "Open / close key", type: "key", default: "F10" }] },
        { label: "Overlay", settings: [
          { key: "allowPanels", label: "Allow mod settings panels", type: "bool", default: true, requires: "reload" },
        ] },
      ] },
  ];

  async function tryFetch(url) {
    try {
      const r = await fetch(url, { cache: "no-store" });
      if (!r.ok) return null;
      return await r.json();
    } catch { return null; }
  }

  // Some plugins register their schema NATIVELY (RegisterSettingsSchema) with
  // the JSON compiled into the DLL as a R"json(...)" literal — there is no
  // settings/<id>.json on disk to fetch. Read the literal out of the plugin
  // source instead (sibling repo, reachable under serve.cmd's root), so the
  // harness shows the exact document the DLL registers. Best-effort: a missing
  // repo or file:// page just skips it.
  async function tryFetchNativeSchema(url) {
    try {
      const r = await fetch(url, { cache: "no-store" });
      if (!r.ok) return null;
      const m = (await r.text()).match(/R"json\(([\s\S]*?)\)json"/);
      return m ? JSON.parse(m[1]) : null;
    } catch { return null; }
  }

  async function loadSources() {
    const loaded = [];
    // Shipped schemas (present when served over http from the repo root-ish).
    for (const rel of ["../../data/OSFUI/settings/osfui.json"]) {
      const s = await tryFetch(rel);
      if (s && s.groups) loaded.push(s);
    }
    // OSF Animation's schema ("osf") — registered natively, extracted from its
    // plugin source so the Mods page shows its settings like in game.
    const osf = await tryFetchNativeSchema("../../../OSF Animation/src/API/UISettings.cpp");
    if (osf && osf.groups) loaded.push(osf);
    // ?schema=<url> override / addition.
    const q = new URLSearchParams(location.search).get("schema");
    if (q) { const s = await tryFetch(q); if (s && s.groups) loaded.push(s); }

    const schemas = loaded.length ? loaded : FALLBACK;
    mods = [];
    schemas.forEach(upsert);
    sendData();
    log("info", `loaded ${mods.length} schema(s): ${mods.map((m) => m.id).join(", ")}`);
  }

  // ---- drag-drop live schema loading ----
  function wireDrop() {
    const stop = (e) => { e.preventDefault(); e.stopPropagation(); };
    ["dragenter", "dragover", "dragleave", "drop"].forEach((ev) =>
      window.addEventListener(ev, stop, false));
    window.addEventListener("drop", (e) => {
      const files = [...(e.dataTransfer && e.dataTransfer.files || [])].filter((f) => f.name.endsWith(".json"));
      let pending = files.length;
      if (!pending) return;
      files.forEach((f) => {
        const reader = new FileReader();
        reader.onload = () => {
          try { const s = JSON.parse(reader.result); if (s.groups) { upsert(s); log("info", `loaded dropped ${s.id || f.name}`); } }
          catch (err) { log("info", `bad JSON in ${f.name}: ${err}`); }
          if (--pending === 0) sendData();
        };
        reader.readAsText(f);
      });
    }, false);
  }

  // ---- install ----
  window.osfui = {
    postMessage(json) {
      let m; try { m = JSON.parse(json); } catch { return; }
      log("←web", (m.payload && m.payload.command) || m.type);
      if (m.type === "ui.command") handle(m.payload);
    },
    onMessage: null,
    // Harness-only helper for automated tests / the top-bar buttons.
    _mock: {
      reset() { mods.forEach((m) => localStorage.removeItem(LS_PREFIX + m.id)); loadSources(); },
      mods: () => mods,
      fixtures: setFixtures,          // toggle (no arg) or set; returns state
      fixturesOn: () => fixturesOn,
    },
  };

  // Seed immediately so an early settings.get is answerable, then upgrade
  // asynchronously once real schemas resolve.
  FALLBACK.forEach(upsert);
  wireDrop();
  loadSources();
})();
