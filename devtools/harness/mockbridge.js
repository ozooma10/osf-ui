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
//
// Localization: the mock mirrors the runtime's i18n.get/i18n.data and the
// LocalizeSchema/views walk. Pick a locale in the top bar (or ?locale=…,
// persisted): "en" is authored text (off), "pseudo" pseudo-localizes every
// localized string ([åççéñŧš] + length padding — hardcoded text and tight
// layouts stand out), and a real locale applies l10n catalogs — fetched from
// the repo's l10n dirs and/or dropped onto the page as <modId>_<locale>.json,
// the same flat address→string files the game loads from
// SFSE/Plugins/OSFUI/l10n/.

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
    const mod = { id, title: schema.title || id, schema, values };
    // Advisory authored-against version (mirrors SettingsStore): carried in
    // settings.data so the harness exercises the "needs update" badge.
    if (typeof schema.targetVersion === "string" && /^[0-9]+(\.[0-9]+){0,2}$/.test(schema.targetVersion)) {
      mod.targetVersion = schema.targetVersion;
    }
    return mod;
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

  // ---- localization mirror (LocalizationService + Runtime i18n) ----
  // Active locale, persisted like fixtures: ?locale=… wins, then localStorage.
  // "en" = authored text (localization off); "pseudo" = pseudo-localization.
  const LOCALE_LS = LS_PREFIX + "locale";
  const localeParam = new URLSearchParams(location.search).get("locale");
  if (localeParam !== null) {
    try { localStorage.setItem(LOCALE_LS, localeParam); } catch { /* ignore */ }
  }
  let locale = (() => { try { return localStorage.getItem(LOCALE_LS) || "en"; } catch { return "en"; } })();

  // Pseudo-loc: accent every letter (glyph coverage), pad ~30% (German-ish
  // expansion), bracket the whole string (a bare-English survivor = a string
  // that never went through the localization path). Text stays readable.
  const PSEUDO_ACCENTS = {
    A: "Å", B: "Ɓ", C: "Ç", D: "Đ", E: "É", F: "Ƒ", G: "Ĝ", H: "Ĥ", I: "Î", J: "Ĵ", K: "Ķ", L: "Ļ", M: "Ṁ",
    N: "Ñ", O: "Ø", P: "Þ", Q: "Ǫ", R: "Ŕ", S: "Š", T: "Ŧ", U: "Û", V: "Ṽ", W: "Ŵ", X: "Ẋ", Y: "Ý", Z: "Ž",
    a: "å", b: "ƀ", c: "ç", d: "đ", e: "é", f: "ƒ", g: "ĝ", h: "ĥ", i: "î", j: "ĵ", k: "ķ", l: "ļ", m: "ṁ",
    n: "ñ", o: "ø", p: "þ", q: "ǫ", r: "ŕ", s: "š", t: "ŧ", u: "û", v: "ṽ", w: "ŵ", x: "ẋ", y: "ý", z: "ž",
  };
  function pseudoize(s) {
    if (typeof s !== "string" || !s) return s;
    const accented = s.replace(/[A-Za-z]/g, (ch) => PSEUDO_ACCENTS[ch] || ch);
    const pad = "·".repeat(Math.min(12, Math.max(1, Math.round(s.length * 0.3))));
    return "[" + accented + pad + "]";
  }

  // Catalogs: flat address→string maps, keyed mod → locale, exactly the
  // <modId>_<locale>.json files the game loads from SFSE/Plugins/OSFUI/l10n/.
  // Fetched from the repo's l10n dirs (best-effort) and/or dropped on the
  // page; a dropped file wins over a fetched one, so a translator can iterate.
  const droppedCatalogs = Object.create(null);  // modId -> { locale -> catalog }
  const catalogCache = new Map();               // "modId|locale" -> object | null
  async function fetchCatalog(modId, loc) {
    const key = modId + "|" + loc;
    if (catalogCache.has(key)) return catalogCache.get(key);
    let found = null;
    for (const dir of ["../../data/OSFUI/l10n/", "../../examples/settings-only/l10n/"]) {
      const j = await tryFetch(dir + modId + "_" + loc + ".json");
      if (j && typeof j === "object" && !Array.isArray(j) && !j.groups) { found = j; break; }
    }
    catalogCache.set(key, found);
    return found;
  }
  // Merged active-locale overrides per mod (native CatalogFor): base language
  // first, exact locale over it (FallbackLocales, minus the "en" tail — "en"
  // here means localization OFF so the harness default stays pristine).
  // Catalog-affecting operations (locale switches, schema (re)loads, i18n.get)
  // serialize through one queue: a locale switch overlapping the async schema
  // load would otherwise build its catalog set from a stale mod list and push
  // an unlocalized settings.data.
  let i18nQueue = Promise.resolve();
  function queued(fn) {
    const p = i18nQueue.then(fn);
    i18nQueue = p.catch(() => { /* keep the queue alive past a failed op */ });
    return p;
  }
  let activeCatalogs = Object.create(null);     // modId -> { address -> string }
  async function refreshCatalogs() {
    const next = Object.create(null);
    if (locale !== "en" && locale !== "pseudo") {
      const chain = [...new Set([locale.split("-")[0], locale])];
      const ids = new Set(mods.map((m) => m.id).concat(views.map((v) => v.mod)));
      for (const id of ids) {
        const merged = Object.create(null);
        let any = false;
        for (const loc of chain) {
          for (const src of [await fetchCatalog(id, loc), (droppedCatalogs[id] || {})[loc]]) {
            if (src) { Object.assign(merged, src); any = true; }
          }
        }
        if (any) next[id] = merged;
      }
    }
    activeCatalogs = next;
  }
  // Per-string resolve, like LocalizationService::Resolve: catalog override,
  // else authored English (pseudo-transformed in pseudo mode).
  function resolverFor(modId) {
    const cat = activeCatalogs[modId];
    return (address, english) => {
      if (cat && Object.prototype.hasOwnProperty.call(cat, address)) return String(cat[address]);
      return locale === "pseudo" ? pseudoize(english) : english;
    };
  }

  // Mirror of SettingsStore's LocalizeSchema: resolve schema text fields at
  // the SAME structural addresses, so a real catalog behaves like in game.
  function resolveField(obj, field, address, resolve) {
    if (obj && typeof obj[field] === "string") obj[field] = resolve(address, obj[field]);
  }
  function localizeSchema(schema, resolve) {
    resolveField(schema, "title", "settings.title", resolve);
    resolveField(schema, "description", "settings.description", resolve);
    ((Array.isArray(schema.inputContexts) && schema.inputContexts) || []).forEach((c, i) => {
      if (c && typeof c === "object") resolveField(c, "label", `inputContexts.${c.id || i}.label`, resolve);
    });
    ((Array.isArray(schema.presets) && schema.presets) || []).forEach((pr, i) => {
      if (!pr || typeof pr !== "object") return;
      const root = `presets.${pr.id || i}`;
      resolveField(pr, "label", root + ".label", resolve);
      resolveField(pr, "description", root + ".description", resolve);
    });
    ((Array.isArray(schema.groups) && schema.groups) || []).forEach((g, gi) => {
      if (!g || typeof g !== "object") return;
      resolveField(g, "label", `groups.${g.id || gi}.label`, resolve);
      ((Array.isArray(g.settings) && g.settings) || []).forEach((item, ii) => {
        if (!item || typeof item !== "object") return;
        if (item.type === "action") {
          const root = `actions.${item.key || ii}`;
          for (const f of ["label", "hint", "confirm"]) resolveField(item, f, `${root}.${f}`, resolve);
        } else if (item.type === "note") {
          resolveField(item, "text", `notes.${item.id || ii}.text`, resolve);
        } else if (item.type === "image") {
          resolveField(item, "caption", `images.${item.id || ii}.caption`, resolve);
        } else if (item.key) {
          const root = `settings.${item.key}`;
          resolveField(item, "label", root + ".label", resolve);
          resolveField(item, "hint", root + ".hint", resolve);
          if (item.format && typeof item.format === "object") {
            resolveField(item.format, "prefix", root + ".format.prefix", resolve);
            resolveField(item.format, "suffix", root + ".format.suffix", resolve);
          }
          if (Array.isArray(item.options) && Array.isArray(item.optionLabels)) {
            const n = Math.min(item.options.length, item.optionLabels.length);
            for (let i = 0; i < n; i++) {
              if (typeof item.options[i] === "string" && typeof item.optionLabels[i] === "string") {
                item.optionLabels[i] = resolve(`${root}.options.${item.options[i]}`, item.optionLabels[i]);
              }
            }
          }
        }
      });
    });
  }
  // Native DataView localizes a COPY per send; the authored originals stay
  // pristine so switching locales never compounds.
  function localizedMods() {
    if (locale === "en") return mods;
    return mods.map((m) => {
      const schema = JSON.parse(JSON.stringify(m.schema));
      localizeSchema(schema, resolverFor(m.id));
      return Object.assign({}, m, { schema, title: schema.title || m.id });
    });
  }

  // Views can't be told "pseudo" through the catalog (it's address→string and
  // they supply inline English), so pseudo mode wraps the shared helper's
  // osfui.t once — every t()/data-i18n resolution passes through it. The
  // helper loads AFTER this script but decorates the same window.osfui, so the
  // wrap happens lazily (first i18n.get / locale change), when t exists.
  let origT = null;
  function installPseudoT() {
    const helper = window.osfui;
    if (!helper) return;
    if (locale === "pseudo") {
      if (!origT && typeof helper.t === "function") {
        origT = helper.t;
        helper.t = (address, english, vars) => pseudoize(origT(address, english, vars));
      }
    } else if (origT) {
      helper.t = origT;
      origT = null;
    }
  }

  // i18n.get subscribes the page (Runtime keeps _i18nSubscribers); the mock
  // hosts one view per page, so one remembered mod domain suffices.
  let i18nMod = null;
  function sendI18nData(requestId) {
    if (i18nMod === null) return;
    send("i18n.data", { mod: i18nMod, locale, strings: activeCatalogs[i18nMod] || {} }, requestId);
  }
  // Mirror of Runtime::RefreshLocalizedData: swap the locale, re-push the
  // catalog to the subscriber, then re-send both localized registries.
  function applyLocale(next) {
    return queued(async () => {
      locale = (typeof next === "string" && next.trim()) ? next.trim() : "en";
      try { localStorage.setItem(LOCALE_LS, locale); } catch { /* ignore */ }
      await refreshCatalogs();
      installPseudoT();  // before the pushes below — their localize() runs use t
      sendI18nData();
      sendData();
      sendViews();
      // Keep the top-bar picker in sync when the switch came from elsewhere
      // (e.g. a dropped catalog auto-activating its locale).
      window.dispatchEvent(new CustomEvent("osfui-mock-locale", { detail: { locale } }));
      log("info", `locale -> ${locale}`);
      return locale;
    });
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
    send("settings.data", { mods: localizedMods(),
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
      .map((v) => {
        const out = Object.assign({ focused: false }, v);
        // Manifest title/description localize natively at views.<name>.title/
        // .description under the owning mod's domain — mirror that here.
        if (locale !== "en") {
          const resolve = resolverFor(v.mod);
          const name = v.id.split("/")[1] || v.id;
          out.title = resolve(`views.${name}.title`, v.title);
          out.description = resolve(`views.${name}.description`, v.description);
        }
        return out;
      }) }, requestId);
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
        const mod = mods.find((m) => m.id === p.mod);
        // Ack shape (items 5 + 11): ok + the authoritative post-clamp `value`,
        // or a machine `code` mirroring SettingsStore::SetWithResult.
        const ack = { mod: p.mod, key: p.key, ok: false };
        if (!mod) {
          ack.code = "unknown-setting";
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
        const mod = mods.find((m) => m.id === p.mod);
        if (!mod) {
          // No longer silent (item 5): a request-carrying caller learns why.
          result(false, { code: "unknown-setting", message: "unknown mod or setting" });
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
      case "i18n.get": {
        // Mirror Runtime's i18n.get: reply i18n.data with the merged
        // active-locale catalog for the mod domain and subscribe the page so
        // a locale change re-pushes. Native defaults `mod` to the calling
        // view's owner — the harness pages are chrome, so "osfui".
        const mod = typeof p.mod === "string" && p.mod ? p.mod : "osfui";
        if (!validModId(mod)) {
          result(false, { code: "invalid-mod", message: "invalid localization mod id" });
          break;
        }
        i18nMod = mod;
        queued(async () => { await refreshCatalogs(); installPseudoT(); sendI18nData(rid); });
        break;
      }
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
      case "osfui.handleBack":
        // In-game this reroutes Esc/pad-B to the page instead of closing the
        // overlay; the harness delivers DOM keys to the page natively anyway,
        // so just ack the grant to keep view boot code warning-free.
        log("info", `osfui.handleBack ${p.handle ? "granted" : "released"} (no-op in harness)`);
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
    await queued(async () => {
      mods = [];
      schemas.forEach(upsert);
      await refreshCatalogs();  // a persisted non-en locale localizes first paint
      sendData();
    });
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
      let droppedLoc = "";
      if (!pending) return;
      files.forEach((f) => {
        const reader = new FileReader();
        reader.onload = () => {
          try {
            const s = JSON.parse(reader.result);
            // l10n catalog by filename, like the native loader's stem parse:
            // <modId>_<locale>.json, content a flat address→string object.
            const cat = /^(.+)_([A-Za-z][A-Za-z0-9-]{0,15})\.json$/.exec(f.name);
            if (s && s.groups) {
              upsert(s);
              log("info", `loaded dropped ${s.id || f.name}`);
            } else if (cat && validModId(cat[1]) && s && typeof s === "object" && !Array.isArray(s)) {
              (droppedCatalogs[cat[1]] = droppedCatalogs[cat[1]] || Object.create(null))[cat[2]] = s;
              droppedLoc = cat[2];
              log("info", `loaded l10n catalog ${cat[1]} [${cat[2]}] — ${Object.keys(s).length} string(s)`);
            } else {
              log("info", `${f.name}: neither a settings schema (groups) nor an l10n catalog (<modId>_<locale>.json)`);
            }
          } catch (err) { log("info", `bad JSON in ${f.name}: ${err}`); }
          if (--pending === 0) {
            // applyLocale re-merges catalogs and re-sends both registries
            // (covering plain schema drops too). A dropped catalog activates
            // its locale when none is selected, so the translation shows up
            // without a second step.
            applyLocale(droppedLoc && locale === "en" ? droppedLoc : locale);
          }
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
      // Get (no arg) or switch the preview locale: "en" = authored (off),
      // "pseudo" = pseudo-loc, anything else applies l10n catalogs. Switching
      // returns a Promise of the applied locale.
      locale(next) { return next === undefined ? locale : applyLocale(String(next)); },
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
      version: await pluginVersion, bridgeVersion: "1.0" });
    send("ui.visibility", { visible: true });
  }, 0);
})();
