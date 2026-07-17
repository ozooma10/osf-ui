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

  // Where a mod's REAL view folder lives, relative to the harness pages. In
  // game every view mounts under one views/ root, so the settings view
  // resolves schema `icon`/`image` assets at ../../<modId>/<file>; from
  // devtools/harness/ that lands nowhere. safeAssetSrc consults this map
  // (root + "/<modId>/<file>") before falling back to "../..". Sibling repo,
  // reachable under serve.cmd's root.
  window.OSFUI_MOD_ASSET_ROOTS = {
    "osf.animation": "../../../OSF Animation/views",
  };

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
      case "flags": {
        // Multi-select over options (item 2). Resolve like native: filter to
        // known options, dedupe, canonicalize to declared-option order.
        if (!Array.isArray(value) || !Array.isArray(setting.options)) return undefined;
        return setting.options.filter((o) => typeof o === "string" && value.includes(o));
      }
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
    return item && ["bool", "int", "float", "enum", "flags", "string", "key"].includes(item.type);
  }

  // Host capabilities this mock claims — mirror src/runtime/Capabilities.h
  // (a mock that lies about capabilities defeats the requires gate AND the
  // runtime.ready `capabilities` handshake, item 6).
  const CAPABILITIES = [
    "settings", "settings.captureKey", "views", "game.calendar", "gamepad",
    "schema:requires", "request-id",
    "type:bool", "type:int", "type:float", "type:enum", "type:string", "type:key", "type:flags",
  ];
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
    // Requires gate (item 2): unmet capabilities register as an inert stub
    // card — no values loaded/served, saved values untouched.
    const missing = Array.isArray(schema.requires)
      ? schema.requires.filter((r) => typeof r !== "string" || !CAPABILITIES.includes(r))
          .map((r) => (typeof r === "string" && r ? r : "(malformed)"))
      : [];
    if (missing.length) {
      return { id, title: schema.title || id, schema, values: {}, stub: true, missingRequires: missing };
    }
    const saved = loadSaved(id);
    const values = {};
    eachSetting(schema, (s) => {
      const v = s.key in saved ? validate(s, saved[s.key]) : undefined;
      values[s.key] = v !== undefined ? v : defaultFor(s);
    });
    return { id, title: schema.title || id, schema, values };
  }

  // Mirror of SettingsStore id validation (api-freeze-plan item 1): mod ids
  // are "<author>.<modname>" — lowercase [a-z0-9-] segments, exactly one dot,
  // max 64 chars. Dotless ids are platform-reserved; "osfui" is the only
  // dotless built-in.
  function validModId(id) {
    return typeof id === "string" &&
           (id === "osfui" || (id.length <= 64 && /^[a-z0-9-]+\.[a-z0-9-]+$/.test(id)));
  }

  function upsert(schema) {
    const mod = buildMod(schema);
    if (!validModId(mod.id)) { log("info", `rejected schema id "${mod.id}" (unsafe or reserved — the store refuses it too)`); return; }
    const i = mods.findIndex((m) => m.id === mod.id);
    if (i >= 0) mods[i] = mod; else mods.push(mod);
  }

  // ---- native -> web ----
  function send(type, payload, requestId) {
    log("→web", type + (requestId ? ` [${requestId}]` : ""));
    if (window.osfui && typeof window.osfui.onMessage === "function") {
      const msg = { type, payload };
      // Item-5 envelope: replies echo the caller's requestId TOP-LEVEL, like
      // MessageBridge::SendToWeb.
      if (requestId) msg.requestId = requestId;
      window.osfui.onMessage(JSON.stringify(msg));
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
  function sendData(requestId) {
    annotateConflicts();
    // Mirror SettingsStore::Data()'s top-level vanillaKeys table (the game's
    // own bindings, full map — the keybinds view renders it).
    send("settings.data", { mods,
      vanillaKeys: VANILLA.map((v) => ({ event: v.event, title: v.title, name: v.name })) }, requestId);
  }

  // The changed setting's fresh conflict list (native: ConflictsForSetting,
  // emitted with key-typed settings.changed, item 11). String compare like
  // annotateConflicts.
  function conflictsForSetting(modId, key) {
    const m = mods.find((x) => x.id === modId);
    const s = findSetting(m, key);
    const v = m && (m.values || {})[key];
    if (!s || !v) return [];
    const expectedGameReuse = blocksGameplay(m.schema, s);
    const others = VANILLA.filter((x) => x.name === v && !expectedGameReuse)
      .map((x) => ({ mod: "@game", key: x.event, title: x.title }));
    for (const other of mods) {
      eachSetting(other.schema, (os) => {
        if (os.type === "key" && (other.values || {})[os.key] === v &&
            (other.id !== modId || os.key !== key)) {
          others.push({ mod: other.id, key: os.key, title: other.title });
        }
      });
    }
    return others;
  }

  // ---- view catalog (panels + HUDs on the Mods surface) ----
  // Mirrors the runtime's views.data push. The real shipped views come first —
  // menu.open on one that has a harness page navigates there, so panel launch
  // works inside the harness. The `fixture: true` entries are fictional: they
  // exercise every state the Mods surface renders (a view owned by a settings
  // mod — `mod` matches a schema id, view-only mods with no schema, a failed
  // load, HUD live / hidden). Hidden by default; toggle with the top-bar
  // "Sample views" button or ?fixtures=1 (persisted in localStorage).
  // View ids are qualified "<modId>/<viewName>" (api-freeze-plan item 1),
  // mirroring the nested views/<modId>/<viewName>/ layout.
  const HARNESS_PAGES = { "osfui/settings": "index.html", "osfui/keybinds": "keybinds.html", "osf.animation/browser": "osf.html" };
  const views = [
    { id: "osfui/settings", title: "Mods", description: "Installed mods — settings, panels and HUD toggles.", mod: "osfui", kind: "menu", interactive: true, hub: false, open: false, focused: false, loadState: "loaded" },
    { id: "osfui/keybinds", title: "Keybinds", description: "Full keyboard map of mod and game bindings.", mod: "osfui", kind: "menu", interactive: true, hub: true, open: false, focused: false, loadState: "loaded" },
    // Real view from the sibling repo (VFS-merged in game). mod "osf.animation"
    // groups it onto the OSF Animation settings page (schema registered
    // natively — see tryFetchNativeSchema). Open lands on osf.html.
    { id: "osf.animation/browser", title: "OSF Animation Browser", description: "Scene browser and launcher — crew, furniture, launch.", mod: "osf.animation", kind: "menu", interactive: true, hub: true, open: false, focused: false, loadState: "loaded" },
    { id: "acme.shipworks/almanac", title: "Ship Almanac", description: "Browse ship modules, mass and performance readouts.", mod: "acme.shipworks", kind: "menu", interactive: true, hub: true, open: false, focused: true, loadState: "loaded", fixture: true },
    { id: "acme.shipworks/hudwidgets", title: "HUD Widgets", description: "Clock and status overlays over the live game.", mod: "acme.shipworks", kind: "hud", interactive: false, hub: true, open: true, loadState: "loaded", fixture: true },
    // targetVersion newer than any real OSF UI — with fixtures on, the rail
    // head shows the "needs update" badge next to the version number.
    { id: "acme.cargo/cargo", title: "Cargo Manifest", description: "Sortable inventory with a live mass budget.", mod: "acme.cargo", kind: "menu", interactive: true, hub: true, open: false, focused: false, loadState: "loaded", targetVersion: "99.0.0", fixture: true },
    { id: "acme.atlas/atlas", title: "Star Atlas", description: "Annotated survey routes and anomalies by system.", mod: "acme.atlas", kind: "menu", interactive: true, hub: true, open: false, focused: false, loadState: "failed", fixture: true },
    { id: "acme.vitals/vitals", title: "Vitals Ring", description: "O2, health and affliction indicators.", mod: "acme.vitals", kind: "hud", interactive: false, hub: true, open: false, loadState: "loaded", fixture: true },
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
  function sendViews(requestId) {
    // `focused` is emitted on EVERY entry like the runtime (HUDs are simply
    // never focused) — the d.ts marks it required.
    send("views.data", { views: views.filter((v) => fixturesOn || !v.fixture)
      .map((v) => Object.assign({ focused: false }, v)) }, requestId);
  }

  // Mirrors SettingsModule subscribe-on-read (protocol 1.0): settings.get
  // subscribes the page; committed values then push as settings.changed.
  // Key-typed pushes carry the recomputed `conflicts` (protocol 1.0).
  let subscribed = false;
  function pushChanged(modId, key, value) {
    if (!subscribed) return;
    setTimeout(() => {
      const payload = { mod: modId, key, value };
      const m = mods.find((x) => x.id === modId);
      const s = findSetting(m, key);
      if (s && s.type === "key") payload.conflicts = conflictsForSetting(modId, key);
      send("settings.changed", payload);
    }, 0);
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
  // `rid` is the ui.command's requestId ("" = fire-and-forget). Every reply
  // echoes it; verb commands with no reply type of their own answer
  // `ui.result { ok, command }` when it was supplied — mirroring
  // MessageBridge's auto-ack (item 5).
  let captureBusy = false;
  function handle(p, rid) {
    const cmd = p && p.command;
    const result = (ok, extra) => {
      if (rid) setTimeout(() => send("ui.result", Object.assign({ ok, command: cmd }, extra || {}), rid), 0);
    };
    switch (cmd) {
      case "settings.get":
        subscribed = true;
        setTimeout(() => sendData(rid), 0);
        break;
      case "settings.set": {
        const mod = mods.find((m) => m.id === p.mod && !m.stub);  // stubs are inert, like native
        // Ack shape (items 5 + 11): ok + the authoritative post-clamp `value`,
        // or a machine `code` mirroring SettingsStore::SetWithResult.
        const ack = { mod: p.mod, key: p.key, ok: false };
        const stubbed = mods.find((m) => m.id === p.mod && m.stub);
        if (!mod) {
          ack.code = stubbed ? "read-only" : "unknown-setting";
        } else {
          let setting = null;
          eachSetting(mod.schema, (s) => { if (s.key === p.key) setting = s; });
          if (!setting) {
            ack.code = "unknown-setting";
          } else {
            const v = validate(setting, p.value);
            if (v === undefined) {
              ack.code = "invalid-value";
            } else {
              mod.values[p.key] = v;
              persist(mod);
              ack.ok = true;
              ack.value = v;
              delete ack.code;
              pushChanged(p.mod, p.key, v); // post-validation value, like native
              pushPersisted(p.mod);
            }
          }
        }
        setTimeout(() => send("settings.ack", ack, rid), 0);
        break;
      }
      case "settings.reset": {
        const mod = mods.find((m) => m.id === p.mod && !m.stub);
        if (!mod) {
          // No longer silent (item 5): a request-carrying caller learns why.
          result(false, { code: "unknown-setting", message: "unknown mod or setting (or a requires-gated stub)" });
          break;
        }
        // Native parity (item 12): NO per-key settings.changed fan-out — the
        // one authoritative settings.data below re-syncs everything.
        eachSetting(mod.schema, (s) => {
          if (!p.key || s.key === p.key) mod.values[s.key] = defaultFor(s);
        });
        persist(mod);
        pushPersisted(p.mod);
        setTimeout(() => sendData(rid), 0); // mirrors SettingsModule: re-send registry
        break;
      }
      case "settings.captureKey": {
        // Captures ANY (mod,key), matching the in-game runtime: native arms
        // capture for every setting a schema declares `type:"key"`. One at a
        // time — a second arm refuses visibly (item 11).
        if (captureBusy) {
          result(false, { code: "capture-busy", message: "a key capture is already in progress" });
          break;
        }
        captureBusy = true;
        const onKey = (e) => {
          window.removeEventListener("keydown", onKey, true);
          captureBusy = false;
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
          // Deferred reply: echoes the ARMING request's id (item 5), like
          // Runtime::DrainKeyCapture.
          send("settings.captured", payload, rid);
        };
        window.addEventListener("keydown", onKey, true);
        break;
      }
      case "views.get":
        setTimeout(() => sendViews(rid), 0);
        break;
      case "game.get":
        // Nested per-provider (item 11): future providers are SIBLINGS of
        // `calendar`. Fixed sample date — enough to render a HUD clock.
        setTimeout(() => send("game.data", { calendar: {
          available: true, day: 12, month: 7, year: 2330, hour: 14.52, daysPassed: 87.3,
        } }, rid), 0);
        break;
      case "menu.open": {
        const page = HARNESS_PAGES[p.view];
        if (page) {
          // Real shipped view — hand off to its harness page (brief delay,
          // like the in-game single-menu swap).
          log("info", `menu.open ${p.view} → ${page}`);
          setTimeout(() => { location.href = page; }, 450);
        } else if (views.some((v) => v.id === p.view)) {
          // Fictional view — mark it open/focused and push, which clears the
          // launch overlay (mirrors the runtime's reconcile push); the verb
          // itself acks via ui.result like native's auto-ack.
          result(true);
          setTimeout(() => {
            for (const v of views) { if (v.kind === "menu") { v.focused = v.id === p.view; v.open = v.open || v.id === p.view; } }
            sendViews();
          }, 400);
        } else {
          result(false, { code: "unknown-view", message: "not a registered surface" });
        }
        break;
      }
      case "hud.show":
      case "hud.hide": {
        const v = views.find((x) => x.id === p.view);
        if (!v) {
          result(false, { code: "unknown-view", message: "not a registered surface" });
          break;
        }
        v.open = cmd === "hud.show";
        result(true);
        setTimeout(() => sendViews(), 150); // async reconcile, like native
        break;
      }
      case "close":
        log("info", "close (no-op in harness)");
        result(true);
        break;
      default:
        // Plugin command shape (item 3): "<author>.<modname>.<name>" — two
        // dots minimum. The mock plays the BRIDGE's part: ui.result ok:true =
        // delivered to the plugin's handler (native auto-ack). Anything else
        // is an unknown command -> ui.error, like MessageBridge.
        if (typeof cmd === "string" && cmd.indexOf(".") > 0 &&
            cmd.indexOf(".", cmd.indexOf(".") + 1) > 0) {
          setTimeout(() => {
            if (rid) send("ui.result", { ok: true, command: cmd, message: "Done (mock)" }, rid);
          }, 400);
        } else {
          send("ui.error", { code: "unknown-command", message: "unknown command",
            command: String(cmd).slice(0, 128) }, rid);
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

  // The real plugin version, read out of src/core/Version.h so the harness
  // version badge shows what the DLL would report (reachable under
  // serve.cmd's root). Best-effort: a file:// page or missing file keeps
  // the "-mock" marker so a stale/fake version is never mistaken for real.
  const pluginVersion = (async () => {
    try {
      const r = await fetch("../../src/core/Version.h", { cache: "no-store" });
      const m = r.ok ? (await r.text()).match(/kPluginVersion\s*=\s*"([^"]+)"/) : null;
      return m ? m[1] : "1.0.0-mock";
    } catch { return "1.0.0-mock"; }
  })();

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
    // OSF Animation's schema — registered natively, extracted from its
    // plugin source so the Mods page shows its settings like in game.
    const osf = await tryFetchNativeSchema("../../../OSF Animation/src/API/UISettings.cpp");
    if (osf && osf.groups) {
      // Stale-checkout shim: remap the pre-migration dotless id (the store
      // rejects "osf"; the sibling repo registers "osf.animation" since its
      // api-freeze migration).
      if (osf.id === "osf") osf.id = "osf.animation";
      loaded.push(osf);
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
      // requestId cap mirrors MessageBridge (<=64 chars, string, else absent).
      const rid = typeof m.requestId === "string" && m.requestId.length > 0 && m.requestId.length <= 64
        ? m.requestId : "";
      if (m.type === "ui.command") handle(m.payload, rid);
    },
    onMessage: null,
    // Harness-only helper for automated tests / the top-bar buttons.
    _mock: {
      reset() { mods.forEach((m) => localStorage.removeItem(LS_PREFIX + m.id)); loadSources(); },
      mods: () => mods,
      fixtures: setFixtures,          // toggle (no arg) or set; returns state
      fixturesOn: () => fixturesOn,
      visibility(v) { send("ui.visibility", { visible: !!v }); },  // fake a show/hide edge
    },
  };

  // Seed immediately so an early settings.get is answerable, then upgrade
  // asynchronously once real schemas resolve.
  FALLBACK.forEach(upsert);
  wireDrop();
  loadSources();

  // Native greets every view on load (SendRuntimeReady) — push runtime.ready
  // proactively like the runtime does, instead of gating it behind views.get
  // (item 12: divergent boot semantics). Deferred a macrotask so the shared
  // helper (loaded after this script) has installed its onMessage. The
  // runtime also pushes ui.visibility on show/hide edges (item 10); the
  // harness has no real overlay, so announce "shown" once at install and
  // expose window.osfui._mock.visibility(v) for exercising the hide path.
  setTimeout(async () => {
    send("runtime.ready", { game: "Starfield", plugin: "OSF UI",
      version: await pluginVersion, bridgeVersion: "1.0", capabilities: CAPABILITIES });
    send("ui.visibility", { visible: true });
  }, 0);
})();
