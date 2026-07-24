// mockbridge.ts — browser stand-in for the OSF UI native bridge. Dev only.
//
// Installs `window.osfui` with a `postMessage` before the shared kit loads, so the
// kit decorates the same object and the view under test takes its normal bridge
// path (settings.get/set/reset/captureKey, views.get, i18n.get, …). Values persist
// to localStorage; every message is logged to the console.
//
// Load order is load-bearing: src/shared-kit/osfui.js defines `available()` as
// `typeof g.postMessage === "function"` and owns `onMessage`. So: this module first
// (postMessage), the kit second (onMessage + request correlation), the view last.
// harness/install-mock.ts exists to make that order an import-statement order in
// main.tsx.
//
// Validation is not re-implemented here: `normalizeValue`/`isSetting` come from
// @lib/settings/normalize and `resolveInputContext` from @lib/settings/inputContext,
// so the harness cannot drift into accepting a value the game refuses.
//
// `data/OSFUI/l10n/` does not exist in this repo; that is the expected state and is
// not warned about. Catalogs come from examples/settings-only/l10n/ and from files
// dropped onto the page.

import type {
  SettingValue,
  Setting,
  SettingsDataPayload,
  SettingsSchema,
  UiGamepadPayload,
} from '@sdk';
import { isSetting, normalizeValue } from '@lib/settings/normalize';
import { resolveInputContext } from '@lib/settings/inputContext';
import { pseudoize } from './i18n-pseudo';
import {
  FALLBACK_SCHEMAS,
  HARNESS_PAGES,
  HEALTH_SCENARIOS,
  MOCK_HEALTH,
  MOCK_VIEWS,
  MOD_ASSET_ROOTS,
  VANILLA_KEYS,
  type MockView,
} from './fixtures';

/** The locale picker's list. */
export const LOCALES: string[] = [
  'en',
  'pseudo',
  'de',
  'fr',
  'es',
  'it',
  'ja',
  'pl',
  'pt-BR',
  'ru',
  'zh-Hans',
];

/** One registered mod, as `settings.data` carries it. */
export interface MockMod {
  id: string;
  title: string;
  schema: SettingsSchema;
  values: Record<string, SettingValue>;
  targetVersion?: string;
}

/** The subset of `Storage` the mock uses; injectable so tests stay hermetic. */
export interface StorageLike {
  getItem(key: string): string | null;
  setItem(key: string, value: string): void;
  removeItem(key: string): void;
}

export interface MockOptions {
  /** Query string to read `?schema`/`?locale`/`?fixtures` from. Default `location.search`. */
  search?: string;
  /** Persistence backing. `null` disables persistence entirely (tests). */
  storage?: StorageLike | null;
  /** Install the drag-drop schema/catalog loader. Default true. */
  drop?: boolean;
  /** Load the real schema sources at install. Default true. */
  autoLoad?: boolean;
  /**
   * Push `runtime.ready` + `ui.visibility` a macrotask after install, as the
   * runtime greets every view (SendRuntimeReady). Default true.
   */
  greet?: boolean;
  /** Id of the view being hosted; resolves commands that omit `view`. */
  selfView?: string;
}

export interface MockApi {
  /** Clear persisted values for every loaded mod and re-read the sources. */
  reset(): void;
  /** Toggle (no arg) or set the fictional sample views. Returns the new state. */
  fixtures(on?: boolean): boolean;
  fixturesOn(): boolean;
  /** Read the active preview locale, or switch it (resolves with the applied one). */
  locale(): string;
  locale(next: string): Promise<string>;
  /** The live mod list — the same objects the mock serves, not copies. */
  mods(): MockMod[];
  /** Fake an overlay show/hide edge. */
  visibility(visible: boolean): void;
  /**
   * Fire a `ui.hotkey` for a `type:"key"` setting. With no arguments it picks the
   * first key-typed setting in the registry (usually the overlay toggle).
   */
  hotkey(mod?: string, key?: string): boolean;
  /** Inject a shoulder-button down edge followed by its release. */
  gamepad(button: 'LB' | 'RB'): void;
  captureArmed(): boolean;
  /** Disarm an armed capture, answering `cancelled: true`. False when none was armed. */
  cancelCapture(): boolean;
  /** Point the omitted-`view` commands at the currently mounted view. */
  setSelfView(id: string): void;
  /** Switch the System Health scenario (no arg = advance the cycle). Returns the new name. */
  health(name?: string): string;
  /** The active System Health scenario name. */
  healthScenario(): string;
  /** Resolves when the initial source load has settled (tests, mostly). */
  loaded(): Promise<void>;
}

type MockHost = Window & typeof globalThis;

/** The event the toolbar listens for when a dropped catalog auto-activates. */
export const LOCALE_EVENT = 'osfui-mock-locale';

// Repo sources. Glob paths are relative to this file (frontend/harness/): `../..`
// is the repo root, `../../..` the parent directory holding sibling repos.
// vite.config.ts `server.fs.allow` covers both. Globs resolve at transform time,
// so no dev-server root ceremony; a missing file yields an empty map.

/** Shipped settings documents — data/OSFUI/settings/*.json. */
const SHIPPED_SCHEMAS = import.meta.glob<SettingsSchema>('../../data/OSFUI/settings/*.json', {
  import: 'default',
});

/**
 * l10n catalogs: flat address->string maps named `<modId>_<locale>.json` — the
 * files the game loads from SFSE/Plugins/OSFUI/l10n/. `data/OSFUI/l10n/` is absent
 * from this list because it does not exist in this repo.
 */
const L10N_CATALOGS = import.meta.glob<Record<string, string>>(
  '../../examples/settings-only/l10n/*.json',
  { import: 'default' },
);

/**
 * OSF Animation registers its schema natively (RegisterSettingsSchema) with the
 * JSON compiled into the DLL as an `R"json(...)json"` literal — there is no
 * settings/<id>.json on disk. Read the literal out of the plugin source so the
 * harness shows the exact document the DLL registers.
 */
const NATIVE_SCHEMA_SOURCE = import.meta.glob<string>(
  '../../../OSF Animation/src/API/UISettings.cpp',
  { query: '?raw', import: 'default' },
);

/** src/core/Version.h — `kPluginVersion` feeds the harness version badge. */
const VERSION_HEADER = import.meta.glob<string>('../../src/core/Version.h', {
  query: '?raw',
  import: 'default',
});

/** Used when the real version cannot be read; the suffix marks it as not real. */
const FALLBACK_VERSION = '1.0.0-mock';

const LS_PREFIX = 'osfui.mock.';
const LOCALE_LS = LS_PREFIX + 'locale';
const FIXTURES_LS = LS_PREFIX + 'fixtures';

/** XInput LB / RB, matching @lib/lifecycle's PAD_LSHOULDER / PAD_RSHOULDER. */
const PAD_BUTTONS: Record<'LB' | 'RB', number> = { LB: 0x0100, RB: 0x0200 };

/**
 * Mirror of SettingsStore id validation: mod ids are "<author>.<modname>" —
 * lowercase [a-z0-9-] segments, exactly one dot, max 64 chars. Dotless ids are
 * platform-reserved; "osfui" is the only dotless built-in.
 */
export function validModId(id: unknown): id is string {
  return (
    typeof id === 'string' &&
    (id === 'osfui' || (id.length <= 64 && /^[a-z0-9-]+\.[a-z0-9-]+$/.test(id)))
  );
}

/** `settings.data`-shaped conflict entry. */
interface ConflictRef {
  mod: string;
  key: string;
  title: string;
}

type CommandPayload = Record<string, unknown>;

function str(p: CommandPayload, field: string): string {
  const v = p[field];
  return typeof v === 'string' ? v : '';
}

/**
 * Map an OS key event onto an OSF UI key name. Not the shipped view's
 * `domKeyName` (@lib/keybinds/domKeyName): this one plays the native side of the
 * capture and supports only the small set the mock can name.
 */
function domKeyName(e: KeyboardEvent): string {
  if (/^F([1-9]|1[0-9]|2[0-4])$/.test(e.key)) return e.key;
  if (/^[a-z]$/i.test(e.key)) return e.key.toUpperCase();
  if (/^[0-9]$/.test(e.key)) return e.key;
  const named: Record<string, string> = {
    ' ': 'Space',
    Enter: 'Enter',
    Tab: 'Tab',
    ArrowUp: 'Up',
    ArrowDown: 'Down',
    ArrowLeft: 'Left',
    ArrowRight: 'Right',
    '`': 'Grave',
  };
  return named[e.key] || '';
}

export function installMock(opts: MockOptions = {}): MockApi {
  const host = window as MockHost;
  const search = opts.search !== undefined ? opts.search : location.search;
  const storage: StorageLike | null =
    opts.storage !== undefined ? opts.storage : safeLocalStorage();
  const params = new URLSearchParams(search);

  let selfView = opts.selfView || 'osfui/settings';
  let mods: MockMod[] = [];

  const log = (dir: string, msg: string) => console.log(`%c[mock ${dir}]`, 'color:#5aa9b8', msg);

  // Asset roots for the settings view's icon/image resolution. Global because
  // @lib/settings/assets reads it off the window.
  (host as unknown as { OSFUI_MOD_ASSET_ROOTS?: Record<string, string> }).OSFUI_MOD_ASSET_ROOTS =
    MOD_ASSET_ROOTS;

  // Persistence

  function loadSaved(id: string): Record<string, unknown> {
    if (!storage) return {};
    try {
      const raw = storage.getItem(LS_PREFIX + id);
      const parsed: unknown = JSON.parse(raw || '{}');
      return parsed && typeof parsed === 'object' && !Array.isArray(parsed)
        ? (parsed as Record<string, unknown>)
        : {};
    } catch {
      return {};
    }
  }

  function persist(mod: MockMod): void {
    if (!storage) return;
    try {
      storage.setItem(LS_PREFIX + mod.id, JSON.stringify(mod.values));
    } catch {
      /* quota / private mode — the harness works without persistence */
    }
  }

  // Schema walking

  function eachSetting(schema: SettingsSchema | undefined, fn: (s: Setting) => void): void {
    const groups = schema && Array.isArray(schema.groups) ? schema.groups : [];
    for (const g of groups) {
      const items = g && Array.isArray(g.settings) ? g.settings : [];
      for (const item of items) if (isSetting(item)) fn(item);
    }
  }

  function findSetting(mod: MockMod | undefined, key: string): Setting | null {
    // Array rather than a captured `let`: TypeScript's control-flow analysis
    // cannot see that the callback ran, so a captured variable narrows back to
    // its initialiser type at the return.
    const found: Setting[] = [];
    eachSetting(mod && mod.schema, (s) => {
      if (s.key === key) found.push(s);
    });
    // Last match wins: a schema declaring the same key twice serves the later
    // declaration.
    return found.length ? (found[found.length - 1] as Setting) : null;
  }

  /**
   * `default` is served as-is when present, `null` otherwise. The `"default" in
   * setting` test matters: a schema that explicitly writes `default: null` keeps
   * null rather than being treated as undeclared.
   */
  function defaultFor(setting: Setting): SettingValue | null {
    return 'default' in setting && setting.default !== undefined ? setting.default : null;
  }

  /**
   * Does this key setting live in a context that asserts blocksGameplay? Such a
   * context omits @game conflicts: reusing a game key there is the expected
   * design, not a collision.
   */
  function blocksGameplay(schema: SettingsSchema | undefined, setting: Setting | null): boolean {
    if (!setting) return false;
    return resolveInputContext(schema, setting).blocksGameplay;
  }

  function buildMod(schema: SettingsSchema): MockMod {
    const id = schema.id || 'mod';
    const saved = loadSaved(id);
    const values: Record<string, SettingValue> = {};
    eachSetting(schema, (s) => {
      // Persisted values go through the same normalizer as a settings.set, so a
      // hand-edited localStorage entry (or a schema whose min/max tightened since
      // it was written) is clamped or refused on load as the store would.
      const v = s.key in saved ? normalizeValue(s, saved[s.key]) : undefined;
      const resolved = v !== undefined ? v : defaultFor(s);
      // `null` means "no default declared" — not a SettingValue, but the views
      // render it as an empty control, so it is served rather than substituting a
      // type-shaped zero.
      values[s.key] = resolved as SettingValue;
    });
    const mod: MockMod = { id, title: schema.title || id, schema, values };
    // Advisory authored-against version (mirrors SettingsStore): carried in
    // settings.data so the harness exercises the "needs update" badge.
    if (typeof schema.targetVersion === 'string' && /^[0-9]+(\.[0-9]+){0,2}$/.test(schema.targetVersion)) {
      mod.targetVersion = schema.targetVersion;
    }
    return mod;
  }

  function upsert(schema: SettingsSchema): void {
    const mod = buildMod(schema);
    if (!validModId(mod.id)) {
      log('info', `rejected schema id "${mod.id}" (unsafe or reserved — the store refuses it too)`);
      return;
    }
    const i = mods.findIndex((m) => m.id === mod.id);
    if (i >= 0) mods[i] = mod;
    else mods.push(mod);
  }

  // Localization

  const localeParam = params.get('locale');
  if (localeParam !== null && storage) {
    try {
      storage.setItem(LOCALE_LS, localeParam);
    } catch {
      /* ignore */
    }
  }
  let locale = readStored(LOCALE_LS) || 'en';

  const droppedCatalogs: Record<string, Record<string, Record<string, string>>> =
    Object.create(null);
  /** Hits only: caching a miss would hide a catalog dropped after that locale was visited. */
  const catalogCache = new Map<string, Record<string, string>>();

  async function fetchCatalog(modId: string, loc: string): Promise<Record<string, string> | null> {
    const cacheKey = modId + '|' + loc;
    const hit = catalogCache.get(cacheKey);
    if (hit) return hit;
    const suffix = `/${modId}_${loc}.json`;
    for (const path of Object.keys(L10N_CATALOGS)) {
      if (!path.endsWith(suffix)) continue;
      const loader = L10N_CATALOGS[path];
      if (!loader) continue;
      try {
        const json = await loader();
        if (json && typeof json === 'object' && !Array.isArray(json)) {
          catalogCache.set(cacheKey, json);
          return json;
        }
      } catch (err) {
        console.warn(`[mock] l10n catalog ${path} failed to load:`, err);
      }
    }
    return null;
  }

  /**
   * Catalog-affecting operations (locale switches, schema (re)loads, i18n.get)
   * serialize through one queue: a locale switch overlapping the async schema load
   * would build its catalog set from a stale mod list and push an unlocalized
   * settings.data.
   */
  let i18nQueue: Promise<unknown> = Promise.resolve();
  function queued<T>(fn: () => Promise<T>): Promise<T> {
    const p = i18nQueue.then(fn);
    i18nQueue = p.catch(() => {
      /* keep the queue alive past a failed op */
    });
    return p;
  }

  let activeCatalogs: Record<string, Record<string, string>> = Object.create(null);

  /**
   * Merged active-locale overrides per mod (native CatalogFor): base language
   * first, exact locale over it (FallbackLocales minus the "en" tail — "en" here
   * means localization off, so the authored strings show through unchanged).
   */
  async function refreshCatalogs(): Promise<void> {
    const next: Record<string, Record<string, string>> = Object.create(null);
    if (locale !== 'en' && locale !== 'pseudo') {
      const base = locale.split('-')[0] || locale;
      const chain = [...new Set([base, locale])];
      const ids = new Set(mods.map((m) => m.id).concat(views.map((v) => v.mod)));
      for (const id of ids) {
        const merged: Record<string, string> = Object.create(null);
        let any = false;
        for (const loc of chain) {
          const dropped = droppedCatalogs[id];
          for (const src of [await fetchCatalog(id, loc), dropped ? dropped[loc] : undefined]) {
            if (src) {
              Object.assign(merged, src);
              any = true;
            }
          }
        }
        if (any) next[id] = merged;
      }
    }
    activeCatalogs = next;
  }

  /**
   * Per-string resolve, like LocalizationService::Resolve: catalog override, else
   * authored English (pseudo-transformed in pseudo mode).
   */
  function resolverFor(modId: string): (address: string, english: string) => string {
    const cat = activeCatalogs[modId];
    return (address, english) => {
      if (cat && Object.prototype.hasOwnProperty.call(cat, address)) return String(cat[address]);
      return locale === 'pseudo' ? String(pseudoize(english)) : english;
    };
  }

  type Resolve = (address: string, english: string) => string;

  function resolveField(
    obj: Record<string, unknown> | undefined,
    field: string,
    address: string,
    resolve: Resolve,
  ): void {
    if (obj && typeof obj[field] === 'string') obj[field] = resolve(address, obj[field] as string);
  }

  /**
   * Mirror of SettingsStore's LocalizeSchema: resolve schema text fields at the
   * same structural addresses, so a real catalog behaves as it does in game.
   */
  function localizeSchema(schema: SettingsSchema, resolve: Resolve): void {
    const s = schema as unknown as Record<string, unknown>;
    resolveField(s, 'title', 'settings.title', resolve);
    resolveField(s, 'description', 'settings.description', resolve);

    const contexts = Array.isArray(schema.inputContexts) ? schema.inputContexts : [];
    contexts.forEach((c, i) => {
      if (c && typeof c === 'object') {
        resolveField(c as unknown as Record<string, unknown>, 'label', `inputContexts.${c.id || i}.label`, resolve);
      }
    });

    const presets = Array.isArray(schema.presets) ? schema.presets : [];
    presets.forEach((pr, i) => {
      if (!pr || typeof pr !== 'object') return;
      const root = `presets.${pr.id || i}`;
      const obj = pr as unknown as Record<string, unknown>;
      resolveField(obj, 'label', root + '.label', resolve);
      resolveField(obj, 'description', root + '.description', resolve);
    });

    const groups = Array.isArray(schema.groups) ? schema.groups : [];
    groups.forEach((g, gi) => {
      if (!g || typeof g !== 'object') return;
      resolveField(g as unknown as Record<string, unknown>, 'label', `groups.${g.id || gi}.label`, resolve);
      const items = Array.isArray(g.settings) ? g.settings : [];
      items.forEach((raw, ii) => {
        if (!raw || typeof raw !== 'object') return;
        const item = raw as unknown as Record<string, unknown>;
        if (item['type'] === 'action') {
          const root = `actions.${item['key'] || ii}`;
          for (const f of ['label', 'hint', 'confirm']) resolveField(item, f, `${root}.${f}`, resolve);
        } else if (item['type'] === 'note') {
          resolveField(item, 'text', `notes.${item['id'] || ii}.text`, resolve);
        } else if (item['type'] === 'image') {
          resolveField(item, 'caption', `images.${item['id'] || ii}.caption`, resolve);
        } else if (item['key']) {
          const root = `settings.${item['key']}`;
          resolveField(item, 'label', root + '.label', resolve);
          resolveField(item, 'hint', root + '.hint', resolve);
          const format = item['format'];
          if (format && typeof format === 'object') {
            const f = format as Record<string, unknown>;
            resolveField(f, 'prefix', root + '.format.prefix', resolve);
            resolveField(f, 'suffix', root + '.format.suffix', resolve);
          }
          const options = item['options'];
          const optionLabels = item['optionLabels'];
          if (Array.isArray(options) && Array.isArray(optionLabels)) {
            const n = Math.min(options.length, optionLabels.length);
            for (let i = 0; i < n; i++) {
              const opt = options[i];
              const label = optionLabels[i];
              if (typeof opt === 'string' && typeof label === 'string') {
                optionLabels[i] = resolve(`${root}.options.${opt}`, label);
              }
            }
          }
        }
      });
    });
  }

  /**
   * Native DataView localizes a copy per send; the authored originals stay
   * untouched so repeated locale switches never compound.
   */
  function localizedMods(): MockMod[] {
    if (locale === 'en') return mods;
    return mods.map((m) => {
      const schema = JSON.parse(JSON.stringify(m.schema)) as SettingsSchema;
      localizeSchema(schema, resolverFor(m.id));
      return Object.assign({}, m, { schema, title: schema.title || m.id });
    });
  }

  /**
   * Views cannot be told "pseudo" through a catalog (it is address->string and
   * they supply inline English), so pseudo mode wraps the shared kit's `osfui.t`
   * once and every t()/data-i18n resolution passes through it. The kit loads after
   * this module but decorates the same window.osfui, so the wrap happens lazily
   * (first i18n.get / locale change), once `t` exists.
   */
  let origT: ((address: string, english: string, vars?: unknown) => string) | null = null;
  function installPseudoT(): void {
    const helper = host.osfui as
      | { t?: (address: string, english: string, vars?: unknown) => string }
      | undefined;
    if (!helper) return;
    if (locale === 'pseudo') {
      if (!origT && typeof helper.t === 'function') {
        origT = helper.t.bind(helper);
        helper.t = (address, english, vars) => String(pseudoize(origT!(address, english, vars)));
      }
    } else if (origT) {
      helper.t = origT;
      origT = null;
    }
  }

  // i18n.get subscribes the page (Runtime keeps _i18nSubscribers); the mock hosts
  // one view per page, so one remembered mod domain suffices.
  let i18nMod: string | null = null;
  function sendI18nData(requestId?: string): void {
    if (i18nMod === null) return;
    send(
      'i18n.data',
      { mod: i18nMod, locale, strings: activeCatalogs[i18nMod] || {} },
      requestId,
    );
  }

  /**
   * Mirror of Runtime::RefreshLocalizedData: swap the locale, re-push the
   * catalog to the subscriber, then re-send both localized registries.
   */
  function applyLocale(next: unknown): Promise<string> {
    return queued(async () => {
      locale = typeof next === 'string' && next.trim() ? next.trim() : 'en';
      if (storage) {
        try {
          storage.setItem(LOCALE_LS, locale);
        } catch {
          /* ignore */
        }
      }
      await refreshCatalogs();
      installPseudoT(); // before the pushes below — their localize() runs use t
      sendI18nData();
      sendData();
      sendViews();
      // Keeps the toolbar picker in sync when the switch came from elsewhere, e.g.
      // a dropped catalog auto-activating its locale.
      host.dispatchEvent(new CustomEvent(LOCALE_EVENT, { detail: { locale } }));
      log('info', `locale -> ${locale}`);
      return locale;
    });
  }

  // Native -> web

  function send(type: string, payload: unknown, requestId?: string): void {
    log('→web', type + (requestId ? ` [${requestId}]` : ''));
    const g = host.osfui as { onMessage?: (json: string) => void } | undefined;
    if (g && typeof g.onMessage === 'function') {
      // Replies echo the caller's requestId at the top level, like
      // MessageBridge::SendToWeb.
      const msg: { type: string; payload: unknown; requestId?: string } = { type, payload };
      if (requestId) msg.requestId = requestId;
      g.onMessage(JSON.stringify(msg));
    }
  }

  // Conflicts

  /**
   * Mirror SettingsStore::Data()'s key-conflict grouping: a key setting whose bound
   * value is also bound elsewhere gets conflicts:[{mod,key,title}]. Native groups
   * by resolved vk; the mock groups by the value string. Recomputed on each send so
   * a rebind that clears a conflict drops the badge.
   */
  function annotateConflicts(): void {
    const byVal = new Map<string, ConflictRef[]>();
    const push = (v: string, ref: ConflictRef) => {
      const list = byVal.get(v);
      if (list) list.push(ref);
      else byVal.set(v, [ref]);
    };
    for (const v of VANILLA_KEYS) push(v.name, { mod: '@game', key: v.event, title: v.title });
    for (const m of mods) {
      eachSetting(m.schema, (s) => {
        if (s.type === 'key') delete s.conflicts;
      });
      eachSetting(m.schema, (s) => {
        if (s.type !== 'key') return;
        const v = m.values[s.key];
        if (!v || typeof v !== 'string') return;
        push(v, { mod: m.id, key: s.key, title: m.title });
      });
    }
    for (const m of mods) {
      eachSetting(m.schema, (s) => {
        if (s.type !== 'key') return;
        const v = m.values[s.key];
        if (!v || typeof v !== 'string') return;
        const expectedGameReuse = blocksGameplay(m.schema, s);
        const others = (byVal.get(v) || []).filter(
          (x) => (x.mod !== m.id || x.key !== s.key) && !(expectedGameReuse && x.mod === '@game'),
        );
        if (others.length) s.conflicts = others;
      });
    }
  }

  /**
   * The changed setting's fresh conflict list (native ConflictsForSetting, emitted
   * with key-typed settings.changed). String compare, like annotateConflicts.
   */
  function conflictsForSetting(modId: string, key: string): ConflictRef[] {
    const m = mods.find((x) => x.id === modId);
    const s = findSetting(m, key);
    const v = m ? m.values[key] : undefined;
    if (!s || !v || typeof v !== 'string') return [];
    const expectedGameReuse = blocksGameplay(m && m.schema, s);
    const others: ConflictRef[] = VANILLA_KEYS.filter(
      (x) => x.name === v && !expectedGameReuse,
    ).map((x) => ({ mod: '@game', key: x.event, title: x.title }));
    for (const other of mods) {
      eachSetting(other.schema, (os) => {
        if (os.type === 'key' && other.values[os.key] === v && (other.id !== modId || os.key !== key)) {
          others.push({ mod: other.id, key: os.key, title: other.title });
        }
      });
    }
    return others;
  }

  function sendData(requestId?: string): void {
    annotateConflicts();
    const payload: SettingsDataPayload = {
      mods: localizedMods(),
      // Mirror SettingsStore::Data()'s top-level vanillaKeys table: the game's own
      // bindings, full map, rendered by the keybinds view.
      vanillaKeys: VANILLA_KEYS.map((v) => ({ event: v.event, title: v.title, name: v.name })),
    };
    send('settings.data', payload, requestId);
  }

  // View catalog

  // A working copy: menu.open / hud.show mutate open/focused, and the fixtures
  // module must stay an unmutated dataset — otherwise a test that installs twice
  // inherits the first install's state.
  const views: MockView[] = MOCK_VIEWS.map((v) => Object.assign({}, v));

  const fixturesParam = params.get('fixtures');
  if (fixturesParam !== null && storage) {
    try {
      storage.setItem(FIXTURES_LS, fixturesParam === '1' ? '1' : '0');
    } catch {
      /* ignore */
    }
  }
  let fixturesOn = readStored(FIXTURES_LS) === '1';

  function setFixtures(on?: boolean): boolean {
    fixturesOn = on === undefined ? !fixturesOn : !!on;
    if (storage) {
      try {
        storage.setItem(FIXTURES_LS, fixturesOn ? '1' : '0');
      } catch {
        /* ignore */
      }
    }
    sendViews();
    return fixturesOn;
  }

  function sendViews(requestId?: string): void {
    const out = views
      .filter((v) => fixturesOn || !v.fixture)
      .map((v) => {
        // Strip the harness-only marker: not part of the protocol, and a view
        // reading views.data must not see a field the runtime cannot produce.
        const { fixture: _fixture, ...entry } = v;
        if (locale !== 'en') {
          // Manifest title/description localize natively at views.<name>.title /
          // .description under the owning mod's domain.
          const resolve = resolverFor(v.mod);
          const name = v.id.split('/')[1] || v.id;
          entry.title = resolve(`views.${name}.title`, v.title);
          entry.description = resolve(`views.${name}.description`, v.description);
        }
        return entry;
      });
    send('views.data', { views: out }, requestId);
  }

  // System Health (protocol 1.4). Same subscribe-on-read contract as
  // settings/views: `diagnostics.get` replies with the snapshot and every later
  // scenario switch pushes to the subscriber.
  let healthScenario = (() => {
    const wanted = params.get('health') || '';
    return Object.prototype.hasOwnProperty.call(MOCK_HEALTH, wanted) ? wanted : 'clean';
  })();
  let healthSubscribed = false;

  function sendHealth(requestId?: string): void {
    send('diagnostics.data', MOCK_HEALTH[healthScenario] ?? MOCK_HEALTH['clean'], requestId);
  }

  /** Switch scenario (no arg = advance the cycle). Returns the new name. */
  function setHealth(name?: string): string {
    if (name && Object.prototype.hasOwnProperty.call(MOCK_HEALTH, name)) {
      healthScenario = name;
    } else if (name === undefined) {
      const at = HEALTH_SCENARIOS.indexOf(healthScenario);
      healthScenario = HEALTH_SCENARIOS[(at + 1) % HEALTH_SCENARIOS.length] as string;
    }
    if (healthSubscribed) sendHealth();
    return healthScenario;
  }

  // Subscriptions

  // Mirrors SettingsModule subscribe-on-read (protocol 1.0): settings.get
  // subscribes the page; committed values then push as settings.changed.
  let subscribed = false;

  function pushChanged(modId: string, key: string, value: SettingValue): void {
    if (!subscribed) return;
    setTimeout(() => {
      const payload: { mod: string; key: string; value: SettingValue; conflicts?: ConflictRef[] } = {
        mod: modId,
        key,
        value,
      };
      const m = mods.find((x) => x.id === modId);
      const s = findSetting(m, key);
      if (s && s.type === 'key') payload.conflicts = conflictsForSetting(modId, key);
      send('settings.changed', payload);
    }, 0);
  }

  /**
   * Mirrors the native write-behind (SettingsStore::PumpPersistence, ~500ms per-mod
   * window opened at the first unflushed change): one settings.persisted push per
   * window confirms the disk write. persist() above is immediate — only the
   * notification is delayed, which is all the view can observe.
   */
  const persistTimers = new Map<string, ReturnType<typeof setTimeout>>();
  function pushPersisted(modId: string): void {
    if (persistTimers.has(modId)) return; // window already open — coalesce
    persistTimers.set(
      modId,
      setTimeout(() => {
        persistTimers.delete(modId);
        if (subscribed) send('settings.persisted', { mod: modId });
      }, 500),
    );
  }

  // Key capture

  interface ArmedCapture {
    mod: string;
    key: string;
    rid: string;
    disarm(): void;
  }
  let capture: ArmedCapture | null = null;

  /**
   * Finish an armed capture. `cancelled` covers Escape, an unbindable key, and the
   * disarm paths: a click elsewhere or a window blur means the user walked away and
   * the game's capture would have ended too. An arm left live swallows an unrelated
   * later keypress and makes every subsequent capture answer `capture-busy`.
   */
  function finishCapture(name: string, cancelled: boolean): void {
    const armed = capture;
    if (!armed) return;
    armed.disarm();
    capture = null;

    const payload: {
      mod: string;
      key: string;
      name: string;
      cancelled: boolean;
      conflicts?: ConflictRef[];
    } = { mod: armed.mod, key: armed.key, name, cancelled };

    // Live-warn during capture: the other key settings already on the captured
    // key, delivered before the view commits (mirrors SettingsStore::ConflictsFor).
    // Omitted when unique, like native.
    if (!cancelled) {
      const targetMod = mods.find((m) => m.id === armed.mod);
      const targetSetting = findSetting(targetMod, armed.key);
      const expectedGameReuse = blocksGameplay(targetMod && targetMod.schema, targetSetting);
      const others: ConflictRef[] = VANILLA_KEYS.filter(
        (v) => v.name === name && !expectedGameReuse,
      ).map((v) => ({ mod: '@game', key: v.event, title: v.title }));
      for (const m of mods) {
        eachSetting(m.schema, (s) => {
          if (s.type === 'key' && m.values[s.key] === name && (m.id !== armed.mod || s.key !== armed.key)) {
            others.push({ mod: m.id, key: s.key, title: m.title });
          }
        });
      }
      if (others.length) payload.conflicts = others;
    }

    // Deferred reply: echoes the arming request's id, like
    // Runtime::DrainKeyCapture.
    send('settings.captured', payload, armed.rid);
  }

  function armCapture(mod: string, key: string, rid: string): void {
    const onKey = (e: KeyboardEvent) => {
      e.preventDefault();
      const name = domKeyName(e);
      finishCapture(name, e.key === 'Escape' || !name);
    };
    // A pointer press outside the capture disarms it. Registered a macrotask late
    // so the click that armed the capture — still propagating when
    // settings.captureKey is handled — cannot cancel it immediately.
    const onPointer = () => finishCapture('', true);
    const onBlur = () => finishCapture('', true);

    let pointerArmed = false;
    const armPointer = setTimeout(() => {
      pointerArmed = true;
      host.addEventListener('pointerdown', onPointer, true);
    }, 0);

    host.addEventListener('keydown', onKey, true);
    host.addEventListener('blur', onBlur);

    capture = {
      mod,
      key,
      rid,
      disarm() {
        clearTimeout(armPointer);
        host.removeEventListener('keydown', onKey, true);
        host.removeEventListener('blur', onBlur);
        if (pointerArmed) host.removeEventListener('pointerdown', onPointer, true);
      },
    };
  }

  // Web -> native

  /**
   * `rid` is the ui.command's requestId ("" = fire-and-forget). Every reply echoes
   * it; verb commands with no reply type of their own answer
   * `ui.result { ok, command }` when it was supplied, mirroring MessageBridge's
   * auto-ack.
   */
  function handle(p: CommandPayload, rid: string): void {
    const cmd = typeof p['command'] === 'string' ? (p['command'] as string) : '';
    const result = (ok: boolean, extra?: Record<string, unknown>) => {
      if (rid) {
        setTimeout(() => send('ui.result', Object.assign({ ok, command: cmd }, extra || {}), rid), 0);
      }
    };
    /** An omitted `view` targets the calling view. */
    const targetView = () => str(p, 'view') || selfView;

    switch (cmd) {
      case 'settings.get':
        subscribed = true;
        setTimeout(() => sendData(rid), 0);
        break;

      case 'settings.set': {
        const modId = str(p, 'mod');
        const key = str(p, 'key');
        const mod = mods.find((m) => m.id === modId);
        // Ack shape: ok + the authoritative post-clamp `value`, or a machine `code`
        // mirroring SettingsStore::SetWithResult.
        const ack: { mod: string; key: string; ok: boolean; value?: SettingValue; code?: string } = {
          mod: modId,
          key,
          ok: false,
        };
        const setting = mod ? findSetting(mod, key) : null;
        if (!mod || !setting) {
          ack.code = 'unknown-setting';
        } else {
          const v = normalizeValue(setting, p['value']);
          if (v === undefined) {
            ack.code = 'invalid-value';
          } else {
            mod.values[key] = v;
            persist(mod);
            ack.ok = true;
            ack.value = v;
            pushChanged(modId, key, v); // post-validation value, like native
            pushPersisted(modId);
          }
        }
        setTimeout(() => send('settings.ack', ack, rid), 0);
        break;
      }

      case 'settings.reset': {
        const modId = str(p, 'mod');
        const key = str(p, 'key');
        const mod = mods.find((m) => m.id === modId);
        if (!mod) {
          result(false, { code: 'unknown-setting', message: 'unknown mod or setting' });
          break;
        }
        // Native parity: no per-key settings.changed fan-out — the single
        // authoritative settings.data below re-syncs everything.
        eachSetting(mod.schema, (s) => {
          if (!key || s.key === key) mod.values[s.key] = defaultFor(s) as SettingValue;
        });
        persist(mod);
        pushPersisted(modId);
        setTimeout(() => sendData(rid), 0); // mirrors SettingsModule: re-send registry
        break;
      }

      case 'settings.captureKey': {
        // Captures any (mod,key), matching the in-game runtime: native arms capture
        // for every setting a schema declares `type:"key"`. One at a time — a
        // second arm refuses.
        if (capture) {
          result(false, { code: 'capture-busy', message: 'a key capture is already in progress' });
          break;
        }
        armCapture(str(p, 'mod'), str(p, 'key'), rid);
        break;
      }

      case 'views.get':
        setTimeout(() => sendViews(rid), 0);
        break;

      case 'diagnostics.get':
        healthSubscribed = true;
        setTimeout(() => sendHealth(rid), 0);
        break;

      case 'osfui.openLogFolder':
        // Payload-free and fixed-target in game; there is nothing to open from a
        // browser, so the harness just proves the command was fired.
        log('info', 'osfui.openLogFolder (no-op in harness)');
        result(true);
        break;

      case 'osfui.openModPage':
        log('info', 'osfui.openModPage (no-op in harness)');
        result(true);
        break;

      case 'i18n.get': {
        // Mirror Runtime's i18n.get: reply i18n.data with the merged active-locale
        // catalog for the mod domain and subscribe the page so a locale change
        // re-pushes. Native defaults `mod` to the calling view's owner — the
        // harness chrome is "osfui".
        const mod = str(p, 'mod') || 'osfui';
        if (!validModId(mod)) {
          result(false, { code: 'invalid-mod', message: 'invalid localization mod id' });
          break;
        }
        i18nMod = mod;
        void queued(async () => {
          await refreshCatalogs();
          installPseudoT();
          sendI18nData(rid);
        });
        break;
      }

      case 'game.get':
        // Nested per-provider: future providers are siblings of `calendar`. Fixed
        // sample date, enough to render a HUD clock.
        setTimeout(
          () =>
            send(
              'game.data',
              {
                calendar: {
                  available: true,
                  day: 12,
                  month: 7,
                  year: 2330,
                  hour: 14.52,
                  daysPassed: 87.3,
                },
              },
              rid,
            ),
          0,
        );
        break;

      case 'ping':
        // `runtime.pong` carries an empty payload and is itself the reply, so
        // there is no additional ui.result auto-ack.
        setTimeout(() => send('runtime.pong', {}, rid), 0);
        break;

      case 'log':
        // Native writes this to OSF UI.log; the console is the harness's log.
        console.log('%c[view log]', 'color:#8b95a1', str(p, 'text'));
        result(true);
        break;

      case 'menu.open': {
        const id = targetView();
        const page = HARNESS_PAGES[id];
        if (page) {
          // Real shipped view — hand off to its harness location, after a brief
          // delay like the in-game single-menu swap.
          log('info', `menu.open ${id} → ${page}`);
          result(true);
          setTimeout(() => {
            location.href = page;
          }, 450);
        } else if (views.some((v) => v.id === id)) {
          // Fictional view — mark it open/focused and push, which clears the launch
          // overlay (mirrors the runtime's reconcile push); the verb itself acks
          // via ui.result like native's auto-ack.
          result(true);
          setTimeout(() => {
            for (const v of views) {
              if (v.kind === 'menu') {
                v.focused = v.id === id;
                v.open = v.open || v.id === id;
              }
            }
            sendViews();
          }, 400);
        } else {
          result(false, { code: 'unknown-view', message: 'not a registered surface' });
        }
        break;
      }

      case 'menu.close': {
        const id = targetView();
        const v = views.find((x) => x.id === id);
        if (!v) {
          result(false, { code: 'unknown-view', message: 'not a registered surface' });
          break;
        }
        v.open = false;
        v.focused = false;
        result(true);
        setTimeout(() => sendViews(), 150); // async reconcile, like native
        break;
      }

      case 'hud.show':
      case 'hud.hide': {
        const v = views.find((x) => x.id === targetView());
        if (!v) {
          result(false, { code: 'unknown-view', message: 'not a registered surface' });
          break;
        }
        v.open = cmd === 'hud.show';
        result(true);
        setTimeout(() => sendViews(), 150); // async reconcile, like native
        break;
      }

      case 'setVisible':
        // Native opens/closes the calling surface; the only thing a page can
        // observe is the visibility edge, so that is what the mock emits.
        result(true);
        setTimeout(() => send('ui.visibility', { visible: p['visible'] === true }), 0);
        break;

      case 'setViewHidden':
        // Per-view hidden state has no field in views.data, so there is nothing to
        // reconcile — the ack is the whole observable behaviour.
        log('info', `setViewHidden ${targetView()} -> ${p['hidden'] === true}`);
        result(true);
        break;

      case 'close':
        log('info', 'close (no-op in harness)');
        result(true);
        break;

      case 'osfui.gamepadRaw':
        // The grant only suppresses the runtime's default pad mapping; the harness
        // has no such mapping (padnav is view-side and unaffected), so it is a
        // no-op here — but it must ack, or every view that asserts it starts up
        // with a rejected request.
        log('info', `osfui.gamepadRaw ${p['raw'] === true ? 'granted' : 'released'} (no-op in harness)`);
        result(true);
        break;

      case 'osfui.handleBack':
        // In game this reroutes Esc/pad-B to the page instead of closing the
        // overlay; the harness delivers DOM keys to the page anyway, so ack the
        // grant to keep view boot code warning-free.
        log('info', `osfui.handleBack ${p['handle'] ? 'granted' : 'released'} (no-op in harness)`);
        result(true);
        break;

      default:
        // Plugin command shape: "<author>.<modname>.<name>" — two dots minimum. The
        // mock plays the bridge's part: ui.result ok:true means delivered to the
        // plugin's handler (native auto-ack). Anything else is an unknown command
        // -> ui.error, like MessageBridge.
        if (cmd.indexOf('.') > 0 && cmd.indexOf('.', cmd.indexOf('.') + 1) > 0) {
          setTimeout(() => {
            if (rid) send('ui.result', { ok: true, command: cmd, message: 'Done (mock)' }, rid);
          }, 400);
        } else {
          send(
            'ui.error',
            {
              code: 'unknown-command',
              message: 'unknown command',
              command: String(p['command']).slice(0, 128),
            },
            rid,
          );
        }
        break;
    }
  }

  // Schema sources

  async function tryFetchJson(url: string): Promise<unknown> {
    try {
      const r = await fetch(url, { cache: 'no-store' });
      if (!r.ok) return null;
      return await r.json();
    } catch {
      return null;
    }
  }

  function hasGroups(v: unknown): v is SettingsSchema {
    return !!v && typeof v === 'object' && Array.isArray((v as SettingsSchema).groups);
  }

  /** Load the first module a glob matched, or null. Warns on the failure path. */
  async function loadOnly<T>(
    glob: Record<string, () => Promise<T>>,
    what: string,
  ): Promise<T | null> {
    const keys = Object.keys(glob);
    const first = keys.length ? glob[keys[0] as string] : undefined;
    if (!first) {
      // Warn loudly: silence here turns a stale checkout or a moved sibling repo
      // into "the harness shows different data" rather than a visible problem.
      console.warn(`[mock] ${what} not found — falling back.`);
      return null;
    }
    try {
      return await first();
    } catch (err) {
      console.warn(`[mock] ${what} failed to load — falling back.`, err);
      return null;
    }
  }

  /**
   * The real plugin version, read out of src/core/Version.h so the harness badge
   * shows what the DLL would report. Best-effort: an unreachable file keeps the
   * "-mock" marker so a fake version is not mistaken for a real one.
   */
  const pluginVersion: Promise<string> = (async () => {
    const text = await loadOnly(VERSION_HEADER, 'src/core/Version.h');
    if (text === null) return FALLBACK_VERSION;
    const m = /kPluginVersion\s*=\s*"([^"]+)"/.exec(text);
    if (!m || !m[1]) {
      console.warn('[mock] Version.h has no kPluginVersion literal — using ' + FALLBACK_VERSION);
      return FALLBACK_VERSION;
    }
    return m[1];
  })();

  /** Extract the `R"json(...)json"` literal a plugin compiles its schema into. */
  async function nativeSchema(): Promise<SettingsSchema | null> {
    const text = await loadOnly(NATIVE_SCHEMA_SOURCE, 'OSF Animation UISettings.cpp');
    if (text === null) return null;
    const m = /R"json\(([\s\S]*?)\)json"/.exec(text);
    if (!m || !m[1]) {
      console.warn('[mock] UISettings.cpp has no R"json(...)json" literal — skipping.');
      return null;
    }
    try {
      return JSON.parse(m[1]) as SettingsSchema;
    } catch (err) {
      console.warn('[mock] UISettings.cpp R"json(...)" literal is not valid JSON — skipping.', err);
      return null;
    }
  }

  async function loadSources(): Promise<void> {
    const loaded: SettingsSchema[] = [];

    for (const path of Object.keys(SHIPPED_SCHEMAS)) {
      const loader = SHIPPED_SCHEMAS[path];
      if (!loader) continue;
      try {
        const s = await loader();
        if (hasGroups(s)) loaded.push(s);
      } catch (err) {
        console.warn(`[mock] shipped schema ${path} failed to load.`, err);
      }
    }
    if (!Object.keys(SHIPPED_SCHEMAS).length) {
      console.warn('[mock] no shipped schemas under data/OSFUI/settings/ — run `npm run build`?');
    }

    const osf = await nativeSchema();
    if (hasGroups(osf)) {
      // Stale-checkout shim: the store rejects the dotless "osf"; the sibling repo
      // registers "osf.animation" since its api-freeze migration.
      if (osf.id === 'osf') osf.id = 'osf.animation';
      loaded.push(osf);
    }

    // ?schema=<url> override / addition. A real fetch: the URL is user-supplied at
    // runtime and so cannot be a build-time glob.
    const q = params.get('schema');
    if (q) {
      const s = await tryFetchJson(q);
      if (hasGroups(s)) loaded.push(s);
      else console.warn(`[mock] ?schema=${q} did not resolve to a settings schema (needs "groups").`);
    }

    const schemas = loaded.length ? loaded : FALLBACK_SCHEMAS;
    await queued(async () => {
      mods = [];
      schemas.forEach(upsert);
      await refreshCatalogs(); // a persisted non-en locale localizes first paint
      sendData();
    });
    log('info', `loaded ${mods.length} schema(s): ${mods.map((m) => m.id).join(', ')}`);
  }

  // Drag-drop live schema loading

  function wireDrop(): void {
    const stop = (e: Event) => {
      e.preventDefault();
      e.stopPropagation();
    };
    for (const ev of ['dragenter', 'dragover', 'dragleave', 'drop']) {
      host.addEventListener(ev, stop, false);
    }
    host.addEventListener(
      'drop',
      (e: Event) => {
        const dt = (e as DragEvent).dataTransfer;
        const files = [...((dt && dt.files) || [])].filter((f) => f.name.endsWith('.json'));
        let pending = files.length;
        let droppedLoc = '';
        if (!pending) return;
        for (const f of files) {
          const reader = new FileReader();
          reader.onload = () => {
            try {
              const s: unknown = JSON.parse(String(reader.result));
              // l10n catalog detected by filename, like the native loader's stem
              // parse: <modId>_<locale>.json, content a flat address->string map.
              const cat = /^(.+)_([A-Za-z][A-Za-z0-9-]{0,15})\.json$/.exec(f.name);
              const modId = cat ? cat[1] : undefined;
              const catLocale = cat ? cat[2] : undefined;
              if (hasGroups(s)) {
                upsert(s);
                log('info', `loaded dropped ${s.id || f.name}`);
              } else if (
                modId &&
                catLocale &&
                validModId(modId) &&
                s &&
                typeof s === 'object' &&
                !Array.isArray(s)
              ) {
                const perMod =
                  droppedCatalogs[modId] || (droppedCatalogs[modId] = Object.create(null));
                perMod[catLocale] = s as Record<string, string>;
                droppedLoc = catLocale;
                log(
                  'info',
                  `loaded l10n catalog ${modId} [${catLocale}] — ${Object.keys(s).length} string(s)`,
                );
              } else {
                log(
                  'info',
                  `${f.name}: neither a settings schema (groups) nor an l10n catalog (<modId>_<locale>.json)`,
                );
              }
            } catch (err) {
              log('info', `bad JSON in ${f.name}: ${String(err)}`);
            }
            if (--pending === 0) {
              // applyLocale re-merges catalogs and re-sends both registries, which
              // covers plain schema drops too. A dropped catalog activates its
              // locale when none is selected.
              void applyLocale(droppedLoc && locale === 'en' ? droppedLoc : locale);
            }
          };
          reader.readAsText(f);
        }
      },
      false,
    );
  }

  // Api

  function firstKeySetting(): { mod: string; key: string } | null {
    for (const m of mods) {
      const keys: string[] = [];
      eachSetting(m.schema, (s) => {
        if (s.type === 'key') keys.push(s.key);
      });
      const first = keys[0];
      if (first !== undefined) return { mod: m.id, key: first };
    }
    return null;
  }

  // Overload signatures live on a function declaration because an object literal
  // member cannot carry them: `MockApi.locale` is read-or-write and the two arms
  // have different return types.
  function localeApi(): string;
  function localeApi(next: string): Promise<string>;
  function localeApi(next?: string): string | Promise<string> {
    return next === undefined ? locale : applyLocale(String(next));
  }

  const api: MockApi = {
    reset() {
      if (storage) for (const m of mods) storage.removeItem(LS_PREFIX + m.id);
      void loadSources();
    },
    mods: () => mods,
    fixtures: setFixtures,
    fixturesOn: () => fixturesOn,
    visibility(v: boolean) {
      send('ui.visibility', { visible: !!v });
    },
    locale: localeApi,
    hotkey(mod?: string, key?: string) {
      const target = mod && key ? { mod, key } : firstKeySetting();
      if (!target) {
        console.warn('[mock] no type:"key" setting in the registry — nothing to fire a hotkey for.');
        return false;
      }
      // Native pushes to every settings.get subscriber; the harness hosts one page,
      // so "subscribed" is the whole audience.
      send('ui.hotkey', { mod: target.mod, key: target.key });
      return true;
    },
    gamepad(button: 'LB' | 'RB') {
      const id = PAD_BUTTONS[button];
      const down: UiGamepadPayload = { kind: 'button', button: { id, down: true } };
      const up: UiGamepadPayload = { kind: 'button', button: { id, down: false } };
      send('ui.gamepad', down);
      // The release matters: @lib/lifecycle's padButtonEdge reports a down edge
      // once per press and needs the up to re-arm, so a down-only injector would
      // fire exactly once per page load.
      setTimeout(() => send('ui.gamepad', up), 0);
    },
    captureArmed: () => capture !== null,
    cancelCapture() {
      if (!capture) return false;
      finishCapture('', true);
      return true;
    },
    setSelfView(id: string) {
      selfView = id;
    },
    health: setHealth,
    healthScenario: () => healthScenario,
    loaded: () => initial,
  };

  // Install

  // Must happen before the shared kit loads: it defines available() as
  // `typeof g.postMessage === "function"` and then takes ownership of onMessage.
  // Decorating rather than replacing keeps whatever the kit already put here if the
  // order ever gets swapped by accident.
  // Cast: Window.osfui is typed as the full injected bridge (postMessage +
  // onMessage both required) because in game it only ever exists fully formed; the
  // mock is what makes it exist, so it starts empty.
  const w = host as unknown as { osfui?: Record<string, unknown> };
  if (!w.osfui) w.osfui = {};
  const g = w.osfui;
  g['postMessage'] = (json: string) => {
    let m: { type?: unknown; payload?: unknown; requestId?: unknown };
    try {
      m = JSON.parse(json);
    } catch {
      return;
    }
    const payload =
      m.payload && typeof m.payload === 'object' ? (m.payload as CommandPayload) : {};
    log('←web', String(payload['command'] || m.type));
    // requestId cap mirrors MessageBridge: string, 1..64 chars, else absent.
    const rid =
      typeof m.requestId === 'string' && m.requestId.length > 0 && m.requestId.length <= 64
        ? m.requestId
        : '';
    if (m.type === 'ui.command') handle(payload, rid);
  };
  if (!('onMessage' in g)) g['onMessage'] = null;
  g['_mock'] = api;

  // Seed immediately so an early settings.get is answerable, then upgrade
  // asynchronously once real schemas resolve.
  FALLBACK_SCHEMAS.forEach(upsert);
  if (opts.drop !== false) wireDrop();
  const initial: Promise<void> = opts.autoLoad === false ? Promise.resolve() : loadSources();

  if (opts.greet !== false) {
    // Native greets every view on load (SendRuntimeReady), so push runtime.ready
    // rather than gating it behind views.get, which would diverge from in-game boot
    // semantics. Deferred a macrotask so the shared kit (loaded after this module)
    // has installed its onMessage. The runtime also pushes ui.visibility on
    // show/hide edges; the harness has no real overlay, so announce "shown" once.
    setTimeout(async () => {
      send('runtime.ready', {
        game: 'Starfield',
        plugin: 'OSF UI',
        version: await pluginVersion,
        bridgeVersion: '1.0',
      });
      send('ui.visibility', { visible: true });
    }, 0);
  }

  return api;

  function readStored(key: string): string {
    if (!storage) return '';
    try {
      return storage.getItem(key) || '';
    } catch {
      return '';
    }
  }
}

/** localStorage, or null where it throws (private mode, sandboxed iframe). */
function safeLocalStorage(): StorageLike | null {
  try {
    return window.localStorage;
  } catch {
    return null;
  }
}
