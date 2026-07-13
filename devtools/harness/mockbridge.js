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
        // Native hard-caps at 256 today; the native slice raises this + honors
        // per-setting maxLength up to 4096. Keep this in lockstep with the store.
        const cap = Math.min(256, setting.maxLength || 256);
        return value.length > cap ? value.slice(0, cap) : value;
      }
      case "key": {
        if (typeof value !== "string" || !value) return undefined;
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

  function upsert(schema) {
    const mod = buildMod(schema);
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
  function sendData() { send("settings.data", { mods }); }

  // Mirrors SettingsModule subscribe-on-read (protocol 0.3): settings.get
  // subscribes the page; committed values then push as settings.changed.
  let subscribed = false;
  function pushChanged(mod, key, value) {
    if (subscribed) setTimeout(() => send("settings.changed", { mod, key, value }), 0);
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
          setTimeout(sendData, 0); // mirrors SettingsModule: re-send registry
        }
        break;
      }
      case "settings.captureKey": {
        // NOTE: this captures ANY (mod,key) — it models the *generalized* key
        // capture the native slice will ship. The current in-game runtime only
        // arms capture for osfui.toggleKey and silently ignores other keys, so a
        // mod key that rebinds fine here will (until then) time out in-game.
        const onKey = (e) => {
          window.removeEventListener("keydown", onKey, true);
          e.preventDefault();
          const name = domKeyName(e);
          send("settings.captured", { mod: p.mod, key: p.key, name, cancelled: e.key === "Escape" || !name });
        };
        window.addEventListener("keydown", onKey, true);
        break;
      }
      case "close":
        log("→native", "close (no-op in harness)");
        break;
      default:
        // Mod action command: reply with a simulated "<mod>.ack" so the button
        // resolves. Real mods do this from their own SFSE plugin.
        if (typeof cmd === "string" && cmd.includes(".") && !cmd.startsWith("ui.") &&
            !cmd.startsWith("settings.") && !cmd.startsWith("menu.") && !cmd.startsWith("hud.")) {
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
          { key: "disableControls", label: "Disable player controls while open", type: "bool", default: true },
          { key: "allowPanels", label: "Allow mod settings panels", type: "bool", default: true, requires: "reload" },
        ] },
        { label: "Cursor", settings: [{ key: "cursorSpeed", label: "Cursor speed", type: "float", min: 0.5, max: 3, step: 0.1, default: 1, format: { suffix: "x", decimals: 1 } }] },
      ] },
  ];

  async function tryFetch(url) {
    try {
      const r = await fetch(url, { cache: "no-store" });
      if (!r.ok) return null;
      return await r.json();
    } catch { return null; }
  }

  async function loadSources() {
    const loaded = [];
    // Shipped schemas (present when served over http from the repo root-ish).
    for (const rel of ["../../data/OSFUI/settings/osfui.json", "../../data/OSFUI/settings/demo.json",
                        "../../examples/settings-only/mymod.json"]) {
      const s = await tryFetch(rel);
      if (s && s.groups) loaded.push(s);
    }
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
    // Harness-only helper for automated tests / a "reset storage" button.
    _mock: {
      reset() { mods.forEach((m) => localStorage.removeItem(LS_PREFIX + m.id)); loadSources(); },
      mods: () => mods,
    },
  };

  // Seed immediately so an early settings.get is answerable, then upgrade
  // asynchronously once real schemas resolve.
  FALLBACK.forEach(upsert);
  wireDrop();
  loadSources();
})();
