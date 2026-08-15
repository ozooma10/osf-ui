// mockbridge.ts — browser stand-in for the OSF UI native bridge (mod API 2.0,
// bridge protocol 2.0). Dev only.
//
// Installs `window.osfui` with a `postMessage` before the shared kit loads, so the
// kit decorates the same object and the view under test takes its normal bridge
// path. Values persist to localStorage; every envelope is logged to the console.
//
// Load order is load-bearing: src/shared-kit/osfui.js decides `available` from
// `typeof g.postMessage === "function"` and owns `onMessage`. So: this module
// first (postMessage), the kit second (onMessage + request correlation), the view
// last. Under `osfui dev`, the harness bootstrap installs a queuing postMessage
// stub before any page script and osfui.mock.ts's install() hands this module the
// takeover, so that order holds for the classic-script pages too.
//
// What it emulates, in protocol terms (docs/mod-api-2.0-design.md):
//
//   web -> here   { kind:"send", name, payload } | { kind:"request", name, id, payload }
//   here -> web   ready | state | event | reply | error
//
// `osfui.hello` is the ONLY boot path: it answers `ready`, replays every state
// key, then opens the event gate and flushes what was queued behind it. Nothing
// is pushed on a timer, so an F5 in the harness takes exactly the path a fresh
// document takes in game.
//
// Kind is enforced, because a mock that shrugs at it lets a view ship a `send`
// to a request endpoint: a request naming a send endpoint gets
// `wrong-endpoint-kind`; a send naming a request endpoint is dropped and
// surfaced as an `osfui.debug.error` event, which the kit prints to the page
// console.
//
// Validation is not re-implemented here: `normalizeValue`/`isSetting` come from
// @lib/settings/normalize and `resolveHotkeyContext` from @lib/settings/hotkeyContext,
// so the harness cannot drift into accepting a value the game refuses. The same
// goes for write authority: `settings.set`/`settings.reset`/`settings.captureKey`
// apply Ids::ResolveWritableMod's rule, so a third-party view that reaches into a
// neighbour's settings fails here the way it fails in game.
//
// `data/OSFUI/l10n/` does not exist in this repo; that is the expected state and is
// not warned about. Catalogs come from examples/settings-only/l10n/ and from files
// dropped onto the page.

import type {
  HandoffState,
  SettingValue,
  Setting,
  SettingsData,
  SettingsSchema,
  UiGamepadPayload,
} from '@sdk';
import { isSetting, normalizeValue } from '@lib/settings/normalize';
import { resolveHotkeyContext } from '@lib/settings/hotkeyContext';
// Plays the native side of the capture with the shipped view's mapper:
// `e.code`-based (physical, layout-independent), exactly how the runtime now
// names keys — so the harness capture agrees with in-game behavior on any
// keyboard layout.
import { domKeyName } from '@lib/keybinds/domKeyName';
import { pseudoize } from '@osfui/cli/pseudo';
import {
  FALLBACK_SCHEMAS,
  HARNESS_PAGES,
  HEALTH_SCENARIOS,
  MOCK_HEALTH,
  MOCK_VIEWS,
  MOD_ASSET_ROOTS,
  GAME_BINDINGS,
  LIVE_KEYBINDINGS,
  type MockView,
} from './fixtures';


/** One registered mod, as the `osfui/settings` state key carries it. */
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
   * Answer the document's `osfui.hello` with the ready/state-replay handshake.
   * Default true. `false` leaves every document ungreeted, which is only useful
   * for exercising the pre-greeting gate — there is no other boot path.
   */
  greet?: boolean;
  /** Id of the view being hosted: names the document in `ready`, owns its i18n domain, and resolves requests that omit `view`. */
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
  /** Fake an overlay show/hide edge (`ui.visibility`). */
  visibility(visible: boolean): void;
  /**
   * Fire a `ui.hotkey` for a `type:"key"` setting. With no arguments it picks the
   * first key-typed setting in the registry (usually the overlay toggle).
   */
  hotkey(mod?: string, key?: string): boolean;
  /** Inject a shoulder-button down edge followed by its release. */
  gamepad(button: 'LB' | 'RB'): void;
  captureArmed(): boolean;
  /** Disarm an armed capture, emitting `settings.captured { cancelled: true }`. False when none was armed. */
  cancelCapture(): boolean;
  /** Point the omitted-`view` requests (and the i18n domain) at the currently mounted view. */
  setSelfView(id: string): void;
  /** Switch the System Health scenario (no arg = advance the cycle). Returns the new name. */
  health(name?: string): string;
  /** The active System Health scenario name. */
  healthScenario(): string;
  /** True once the document has greeted: state is published and events flow. */
  greeted(): boolean;
  /** Resolves when the initial source load (and the version read) has settled. */
  loaded(): Promise<void>;
}

type MockWindow = Window & typeof globalThis;

/** The event the toolbar listens for when a dropped catalog auto-activates. */
export const LOCALE_EVENT = 'osfui-mock-locale';

// Repo sources. Glob paths are relative to this file (frontend/devmock/): `../..`
// is the repo root, `../../..` the parent directory holding sibling repos. The
// `osfui dev` server serves both (fs.strict is off). Globs resolve at transform
// time, so no dev-server root ceremony; a missing file yields an empty map.

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

/** src/Core/Version.h — `kOsfuiReleaseVersion` feeds the harness OSF UI release-version badge. */
const VERSION_HEADER = import.meta.glob<string>('../../src/Core/Version.h', {
  query: '?raw',
  import: 'default',
});

/** Used when the real version cannot be read; the suffix marks it as not real. */
const FALLBACK_OSFUI_RELEASE_VERSION = '1.0.0-mock';

/** `bridgeVersion` in the `ready` payload — the protocol this file speaks. */
const BRIDGE_VERSION = '2.0';

const LS_PREFIX = 'osfui.mock.';
const LOCALE_LS = LS_PREFIX + 'locale';
const FIXTURES_LS = LS_PREFIX + 'fixtures';

/** The platform's first-load handoff view; the only view served `osfui/handoff`. */
const HANDOFF_VIEW = 'osfui/handoff';

/** OSF UI's own settings views — the only views allowed to write a foreign mod (Ids::IsSettingsEditorView). */
const SETTINGS_EDITOR_VIEWS = ['osfui/settings', 'osfui/keybinds'];

/**
 * Bridge bounds, mirrored from MessageBridge.cpp so the harness refuses exactly
 * what the runtime refuses: request ids are 1..64 chars, echoed endpoint names
 * are truncated, and events raised before a document greets are held (oldest
 * dropped) rather than shouted at a page with no listeners.
 */
const MAX_REQUEST_ID_LENGTH = 64;
const MAX_ECHOED_NAME_LENGTH = 128;
const MAX_QUEUED_EVENTS = 64;

/** XInput LB / RB, matching @lib/lifecycle's PAD_LSHOULDER / PAD_RSHOULDER. */
const PAD_BUTTONS: Record<'LB' | 'RB', number> = { LB: 0x0100, RB: 0x0200 };

/**
 * Endpoints the platform registers, split by KIND — because the kind is what the
 * caller dispatches on, and a mock that got it wrong would let a view ship a
 * `send` to a request endpoint that only fails against the real runtime. Mirrors
 * Runtime::RegisterEndpoints + SettingsModule::RegisterEndpoints.
 */
const SEND_ENDPOINTS = new Set([
  'osfui.hello',
  'close',
  'setVisible',
  'view.ready',
  'log',
  'osfui.gamepadRaw',
  'osfui.handleBack',
  'osfui.handoffRetry',
  'papyrus.call', 'papyrus.send',
]);

const REQUEST_ENDPOINTS = new Set([
  'menu.open',
  'menu.close',
  'setViewHidden',
  'ping',
  'game.get',
  'settings.set',
  'settings.reset',
  'settings.captureKey',
  'osfui.openModPage',
  'osfui.openLogFolder',
  'osfui.setViewAutoStart',
  'papyrus.request',
]);

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

/** The owning mod of a qualified view id ("<modId>/<viewName>"), like Ids::ModOf. */
function modOf(viewId: string): string {
  const slash = viewId.indexOf('/');
  return slash < 0 ? viewId : viewId.slice(0, slash);
}

/**
 * A mod-registered endpoint: "<author>.<modname>.<name>", so two dots minimum.
 * One dot is a typo'd platform endpoint, not a plugin.
 */
function isPluginEndpoint(name: string): boolean {
  const first = name.indexOf('.');
  return first > 0 && name.indexOf('.', first + 1) > first + 1;
}

/** `osfui/settings`-shaped conflict entry. */
interface ConflictRef {
  mod: string;
  key: string;
  title: string;
}

type CommandPayload = Record<string, unknown>;

/** Native -> web envelopes, exactly as MessageBridge's encoders emit them. */
type Envelope =
  | { kind: 'ready'; payload: Record<string, unknown> }
  | { kind: 'state'; mod: string; key: string; value: unknown }
  | { kind: 'event'; name: string; payload: unknown }
  | { kind: 'reply'; id: string; payload: unknown }
  | { kind: 'error'; id: string; payload: { code: string; message: string } };

function str(p: CommandPayload, field: string): string {
  const v = p[field];
  return typeof v === 'string' ? v : '';
}


export function installMock(opts: MockOptions = {}): MockApi {
  const browserWindow = window as MockWindow;
  const search = opts.search !== undefined ? opts.search : location.search;
  const storage: StorageLike | null =
    opts.storage !== undefined ? opts.storage : safeLocalStorage();
  const params = new URLSearchParams(search);

  let selfView = opts.selfView || 'osfui/settings';
  let mods: MockMod[] = [];

  const log = (dir: string, msg: string) => console.log(`%c[mock ${dir}]`, 'color:#5aa9b8', msg);

  /**
   * Transient on-screen note for a request whose real effect is outside the
   * browser. The console line alone is not enough: a button like "Open log
   * folder" is a no-op here by nature, so without visible feedback there is no
   * way to tell a wired button from a dead one. Harness chrome, never a view.
   */
  function notify(msg: string): void {
    const doc = (browserWindow as unknown as { document?: Document }).document;
    if (!doc || !doc.body) return;
    const el = doc.createElement('div');
    el.className = 'harness-toast';
    el.textContent = msg;
    doc.body.appendChild(el);
    setTimeout(() => el.remove(), 2600);
  }

  // Asset roots for the settings view's icon/image resolution. Global because
  // @lib/settings/assets reads it off the window.
  (browserWindow as unknown as { OSFUI_MOD_ASSET_ROOTS?: Record<string, string> }).OSFUI_MOD_ASSET_ROOTS =
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
    return resolveHotkeyContext(schema, setting).blocksGameplay;
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
    // Advisory authored-against version (mirrors SettingsStore): carried in the
    // settings state key so the harness exercises the "needs update" badge.
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

  /**
   * Ids::ResolveWritableMod: the mod a settings write from this document may
   * target. Only OSF UI's own Mod Settings and Keybindings views may name a foreign
   * mod; every other view is confined to its own, and an omitted `mod` resolves
   * to its own rather than being refused. `null` = refuse with "forbidden".
   */
  function writableMod(requested: string): string | null {
    if (SETTINGS_EDITOR_VIEWS.includes(selfView)) return requested;
    const own = modOf(selfView);
    if (!requested || requested === own) return own;
    return null;
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
   * Catalog-affecting operations (locale switches, schema (re)loads, the hello
   * replay) serialize through one queue: a locale switch overlapping the async
   * schema load would build its catalog set from a stale mod list and publish an
   * unlocalized settings registry.
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
      const ids = new Set(
        mods.map((m) => m.id).concat(views.map((v) => v.mod), [modOf(selfView)]),
      );
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

  /** The kit's i18n namespace, as far as the pseudo wrap needs to see it. */
  type PseudoTarget = { t?: (address: string, english: string, vars?: unknown) => string };

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

    const pages = Array.isArray(schema.pages) ? schema.pages : [];
    pages.forEach((p, i) => {
      if (p && typeof p === 'object') {
        resolveField(p as unknown as Record<string, unknown>, 'label', `pages.${p.id || i}.label`, resolve);
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
   * Native DataView localizes a copy per publish; the authored originals stay
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
   * they supply inline English), so pseudo mode wraps the shared kit's
   * `osfui.i18n.t` once and every t()/data-i18n resolution passes through it. The
   * kit loads after this module but decorates the same window.osfui, so the wrap
   * happens lazily (first greeting / locale change), once `t` exists.
   */
  let origT: ((address: string, english: string, vars?: unknown) => string) | null = null;
  function installPseudoT(): void {
    const helper = (browserWindow.osfui as unknown as { i18n?: PseudoTarget } | undefined)?.i18n;
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

  /**
   * Mirror of Runtime::RefreshLocalizedData: swap the locale, then re-publish the
   * catalog and both localized registries as state.
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
      installPseudoT(); // before the publishes below — their localize() runs use t
      publishI18n();
      publishSettings();
      publishViews();
      // Keeps the toolbar picker in sync when the switch came from elsewhere, e.g.
      // a dropped catalog auto-activating its locale.
      browserWindow.dispatchEvent(new CustomEvent(LOCALE_EVENT, { detail: { locale } }));
      log('info', `locale -> ${locale}`);
      return locale;
    });
  }

  // Native -> web

  function label(env: Envelope): string {
    switch (env.kind) {
      case 'ready':
        return 'ready';
      case 'state':
        return `state ${env.mod}/${env.key}`;
      case 'event':
        return `event ${env.name}`;
      case 'reply':
        return `reply [${env.id}]`;
      default:
        return `error [${env.id}] ${env.payload.code}`;
    }
  }

  function deliver(env: Envelope): void {
    log('→web', label(env));
    const g = browserWindow.osfui as { onMessage?: (json: string) => void } | undefined;
    if (g && typeof g.onMessage === 'function') g.onMessage(JSON.stringify(env));
  }

  // Greeting gate. Mirrors MessageBridge's per-view gate: state addressed to an
  // ungreeted document is DROPPED (the hello replay carries every current value,
  // and queueing would risk delivering a stale one after a newer), events are
  // QUEUED oldest-dropped (they are one-shot happenings the page still wants).
  // Two flags, not one, exactly as native does it: state must flow DURING the
  // replay while events stay shut until after it, so an event raised by a
  // replay listener cannot overtake the backlog queued before the greeting.
  let greeted = false;     // state may flow
  let eventsOpen = false;  // events may flow
  let helloSeq = 0;
  const queuedEvents: Envelope[] = [];

  function raise(name: string, payload: unknown): void {
    const env: Envelope = { kind: 'event', name, payload };
    if (!eventsOpen) {
      if (queuedEvents.length >= MAX_QUEUED_EVENTS) queuedEvents.shift();
      queuedEvents.push(env);
      return;
    }
    deliver(env);
  }

  function publish(mod: string, key: string, value: unknown): void {
    if (!greeted) return;
    deliver({ kind: 'state', mod, key, value });
  }

  function respond(id: string, payload: unknown): void {
    deliver({ kind: 'reply', id, payload: payload === undefined ? {} : payload });
  }

  function rejectRequest(id: string, code: string, message: string): void {
    deliver({ kind: 'error', id, payload: { code, message } });
  }

  /**
   * Runtime-detected protocol misuse, handed straight back to the offending document
   * (Runtime::OnProtocolFault in developer mode — and the harness runs in
   * developer mode by definition). The shared kit prints it to this page's console, so a dropped
   * `send` is never silent.
   */
  function reportProtocolFault(code: string, message: string, detail?: Record<string, unknown>): void {
    log('info', `${code} — ${message}`);
    raise('osfui.debug.error', { code, message, detail: { view: selfView, ...(detail || {}) } });
  }

  // Conflicts

  /**
   * Mirror SettingsStore::Data()'s key-conflict grouping: a key setting whose bound
   * value is also bound elsewhere gets conflicts:[{mod,key,title}]. Native groups
   * by resolved vk; the mock groups by the value string. Recomputed on each publish
   * so a rebind that clears a conflict drops the badge.
   */
  function annotateConflicts(): void {
    const byVal = new Map<string, ConflictRef[]>();
    const push = (v: string, ref: ConflictRef) => {
      const list = byVal.get(v);
      if (list) list.push(ref);
      else byVal.set(v, [ref]);
    };
    for (const v of GAME_BINDINGS) push(v.name, { mod: '@game', key: v.event, title: v.title });
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
    const others: ConflictRef[] = GAME_BINDINGS.filter(
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

  // Platform state keys

  // Preview stand-in for native's keycap-label map: filled asynchronously from
  // the browser's own layout (navigator.keyboard.getLayoutMap — Chromium,
  // main frame, secure context) so the harness board shows the developer's
  // real keycaps. Absent everywhere else; every consumer falls back.
  let keyboardLabels: SettingsData['keyboard'];
  {
    const nav = navigator as Navigator & {
      keyboard?: { getLayoutMap?: () => Promise<Map<string, string>> };
    };
    nav.keyboard?.getLayoutMap?.()
      .then((map) => {
        const labels: Record<string, string> = {};
        for (const [code, value] of map) {
          // The map keys are KeyboardEvent.code values; reuse the shipped
          // code->name mapper so the vocabulary matches native exactly.
          const name = domKeyName({ key: '', code });
          if (!name || !value) continue;
          const glyph = value.length === 1 ? value.toUpperCase() : value;
          if (!(name in labels)) labels[name] = glyph;
        }
        if (Object.keys(labels).length) {
          keyboardLabels = { layout: navigator.language || '', labels };
          publishSettings();
        }
      })
      .catch(() => undefined);
  }

  function publishSettings(): void {
    annotateConflicts();
    const value: SettingsData = {
      mods: localizedMods(),
    };
    if (keyboardLabels) value.keyboard = keyboardLabels;
    publish('osfui', 'settings', value);
  }

  function publishInputMap(): void {
    publish('osfui', 'keybindings', LIVE_KEYBINDINGS);
    publish('osfui', 'input-context', {
      available: true,
      revision: 1,
      mode: 'onFoot',
      contexts: [{ id: 0, name: 'MainGameplay' }],
    });
  }

  // View catalog

  // A working copy: menu.open / menu.close mutate open/focused, and the fixtures
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
    publishViews();
    return fixturesOn;
  }

  function publishViews(): void {
    const out = views
      .filter((v) => fixturesOn || !v.fixture)
      .map((v) => {
        // Strip the harness-only marker: not part of the protocol, and a view
        // reading the catalog must not see a field the runtime cannot produce.
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
    publish('osfui', 'views', { views: out });
  }

  // System Health. The `osfui/diagnostics` state key: replayed on greeting and
  // re-published on every scenario switch.
  let healthScenario = (() => {
    const wanted = params.get('health') || '';
    return Object.prototype.hasOwnProperty.call(MOCK_HEALTH, wanted) ? wanted : 'clean';
  })();

  function publishHealth(): void {
    publish('osfui', 'diagnostics', MOCK_HEALTH[healthScenario] ?? MOCK_HEALTH['clean']);
  }

  /** Switch scenario (no arg = advance the cycle). Returns the new name. */
  function setHealth(name?: string): string {
    if (name && Object.prototype.hasOwnProperty.call(MOCK_HEALTH, name)) {
      healthScenario = name;
    } else if (name === undefined) {
      const at = HEALTH_SCENARIOS.indexOf(healthScenario);
      healthScenario = HEALTH_SCENARIOS[(at + 1) % HEALTH_SCENARIOS.length] as string;
    }
    publishHealth();
    return healthScenario;
  }

  /**
   * The i18n catalog is computed PER DOCUMENT: a view's catalog is its owning
   * mod's, which is why this one key carries a different value to each view.
   */
  function publishI18n(): void {
    const mod = modOf(selfView);
    publish('osfui', 'i18n', { mod, locale, strings: activeCatalogs[mod] || {} });
  }

  // Handoff. Platform-private: only the built-in handoff view is ever served
  // this key, so the harness models it only while that view is the one hosted.
  let handoffTimer: ReturnType<typeof setTimeout> | undefined;
  let handoff: HandoffState = (() => {
    const wanted = params.get('handoff') || '';
    const phase: HandoffState['phase'] =
      wanted === 'retrying' || wanted === 'error' ? wanted : 'linking';
    return {
      target: 'acme.shipworks/almanac',
      mod: 'acme.shipworks',
      title: 'Almanac',
      accent: '#3aa9c0',
      phase,
      retry: phase === 'error',
    };
  })();

  function publishHandoff(): void {
    if (selfView !== HANDOFF_VIEW) return;
    publish('osfui', 'handoff', handoff);
  }

  // Change / persist notifications

  function pushChanged(modId: string, key: string, value: SettingValue): void {
    const payload: { mod: string; key: string; value: SettingValue; conflicts?: ConflictRef[] } = {
      mod: modId,
      key,
      value,
    };
    const m = mods.find((x) => x.id === modId);
    const s = findSetting(m, key);
    if (s && s.type === 'key') payload.conflicts = conflictsForSetting(modId, key);
    raise('settings.changed', payload);
  }

  /**
   * Mirrors the native write-behind (SettingsStore::PumpPersistence, ~500ms per-mod
   * window opened at the first unflushed change): one settings.persisted event per
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
        raise('settings.persisted', { mod: modId });
      }, 500),
    );
  }

  // Key capture

  interface ArmedCapture {
    mod: string;
    key: string;
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
      const others: ConflictRef[] = GAME_BINDINGS.filter(
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

    // A one-shot happening, so an EVENT and not the arming request's reply:
    // `settings.captureKey` already settled in machine time with `armed: true`
    // (Runtime::DrainKeyCapture).
    raise('settings.captured', payload);
  }

  function armCapture(mod: string, key: string): void {
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
      browserWindow.addEventListener('pointerdown', onPointer, true);
    }, 0);

    browserWindow.addEventListener('keydown', onKey, true);
    browserWindow.addEventListener('blur', onBlur);

    capture = {
      mod,
      key,
      disarm() {
        clearTimeout(armPointer);
        browserWindow.removeEventListener('keydown', onKey, true);
        browserWindow.removeEventListener('blur', onBlur);
        if (pointerArmed) browserWindow.removeEventListener('pointerdown', onPointer, true);
      },
    };
  }

  // Handshake

  /**
   * Answer `osfui.hello`: ready, then every current state value, then events —
   * MessageBridge::HandleHello. Running on the page's greeting rather than on a
   * timer is what makes an F5 identical to a first open: the harness cannot push
   * a greeting the document missed, because the document asks for it.
   *
   * Async because the OSF UI release-version badge reads src/Core/Version.h and the catalogs may
   * still be loading; `helloSeq` drops a stale greeting whose document has already
   * been replaced by a newer one.
   */
  function greet(): void {
    const seq = ++helloSeq;
    // The queue is NOT cleared. What it holds are the events raised between
    // this view coming up and its document greeting us — the harness mirror of
    // the native ABI's message-before-first-paint guarantee, which
    // MessageBridge::HandleHello flushes rather than discards (covered by
    // tests/native/bridge_api_tests.cpp). A genuine re-greeting has no backlog
    // anyway: its gate was open, so its events went straight out.
    greeted = false;
    eventsOpen = false;
    void osfuiReleaseVersion.then((releaseVersion) =>
      queued(async () => {
        if (seq !== helloSeq) return;
        await refreshCatalogs();
        installPseudoT();
        if (seq !== helloSeq) return;

        deliver({
          kind: 'ready',
          payload: {
            game: 'Starfield',
            plugin: 'OSF UI',
            version: releaseVersion,
            bridgeVersion: BRIDGE_VERSION,
            view: selfView,
            mod: modOf(selfView),
          },
        });
        // State opens BEFORE the replay — that replay is the whole reason the
        // gate exists — while events stay shut through it, so anything a replay
        // listener raises lands BEHIND the pre-greeting backlog instead of
        // overtaking it. The page sees ready, then all state, then every event
        // in the order it actually happened.
        greeted = true;
        publishSettings();
        publishInputMap();
        publishViews();
        publishHealth();
        publishI18n();
        publishHandoff();
        eventsOpen = true;
        for (const env of queuedEvents.splice(0)) deliver(env);
        // The harness has no real overlay, so announce "shown" once per document —
        // the edge a view arms its per-visit state off.
        raise('ui.visibility', { visible: true, reason: 'overlay' });
        log('info', `greeted ${selfView} — ready, state replay, events open`);
      }),
    );
  }

  // Web -> native

  function dispatchSend(name: string, p: CommandPayload): void {
    if (name === 'osfui.hello') {
      if (opts.greet === false) {
        log('info', 'osfui.hello ignored (greet:false)');
        return;
      }
      greet();
      return;
    }
    if (!SEND_ENDPOINTS.has(name)) {
      if (REQUEST_ENDPOINTS.has(name)) {
        // Kind enforcement: running a mutation whose kind the caller got wrong
        // invites worse bugs, so the send is DROPPED — but never silently.
        reportProtocolFault(
          'wrong-endpoint-kind',
          `'${name}' is a request endpoint — use request(), not send()`,
          { name },
        );
        return;
      }
      if (isPluginEndpoint(name)) {
        // A mod's own RegisterSend endpoint. The mock plays the bridge's part:
        // delivered to the plugin's handler, and a send has nothing to answer.
        log('info', `${name} → delivered to the mod's send handler (mock)`);
        return;
      }
      reportProtocolFault('unknown-endpoint', 'no such endpoint', { name });
      return;
    }

    switch (name) {
      case 'close':
        log('info', 'close (no-op in harness)');
        break;

      case 'setVisible':
        // Native opens/closes the calling view; the only thing a page can
        // observe is the visibility edge, so that is what the mock emits.
        raise('ui.visibility', { visible: p['visible'] === true, reason: 'overlay' });
        break;

      case 'view.ready':
        // Manifests with readySignal:true hold the handoff until this arrives.
        log('info', `view.ready — ${selfView} declared meaningful readiness`);
        break;

      case 'log':
        // Native writes this to OSF UI.log; the console is the harness's log.
        console.log('%c[view log]', 'color:#8b95a1', str(p, 'text'));
        break;

      case 'osfui.gamepadRaw':
        // The grant only suppresses the runtime's default pad mapping; the harness
        // has no such mapping (padnav is view-side and unaffected), so it is a
        // no-op here.
        log('info', `osfui.gamepadRaw ${p['raw'] === true ? 'granted' : 'released'} (no-op in harness)`);
        break;

      case 'osfui.handleBack':
        // In game this reroutes Esc/pad-B to the page instead of closing the
        // overlay; the harness delivers DOM keys to the page anyway.
        log('info', `osfui.handleBack ${p['handle'] ? 'granted' : 'released'} (no-op in harness)`);
        break;

      case 'osfui.handoffRetry': {
        if (selfView !== HANDOFF_VIEW) {
          reportProtocolFault('forbidden', 'osfui.handoffRetry is a platform action', { name });
          break;
        }
        // Play the retry the runtime would: back to "retrying", then fail again so
        // the view's error affordance stays reachable.
        handoff = { ...handoff, phase: 'retrying', retry: false };
        publishHandoff();
        clearTimeout(handoffTimer);
        handoffTimer = setTimeout(() => {
          handoff = { ...handoff, phase: 'error', retry: true };
          publishHandoff();
        }, 1200);
        break;
      }

      case 'papyrus.call':
        log(
          'info',
          `papyrus.call ${str(p, 'script')}.${str(p, 'function')} (no Papyrus VM in the harness)`,
        );
        break;

      case 'papyrus.send':
        // The mod is derived from the source view, never the payload — a view
        // cannot reach into another mod's listeners.
        log(
          'info',
          `papyrus.send ${modOf(selfView)}."${str(p, 'name')}" (no Papyrus VM in the harness)`,
        );
        break;

      default:
        break;
    }
  }

  function dispatchRequest(name: string, id: string, p: CommandPayload): void {
    // A request settles EXACTLY once, like MessageBridge: a late or duplicate
    // answer from a deferred handler is dropped rather than delivered twice.
    let settled = false;
    const ok = (payload?: unknown): void => {
      if (settled) return;
      settled = true;
      respond(id, payload === undefined ? {} : payload);
    };
    const fail = (code: string, message: string): void => {
      if (settled) return;
      settled = true;
      rejectRequest(id, code, message);
    };
    /** An omitted `view` targets the calling view. */
    const targetView = () => str(p, 'view') || selfView;

    if (!REQUEST_ENDPOINTS.has(name)) {
      if (SEND_ENDPOINTS.has(name)) {
        fail('wrong-endpoint-kind', `'${name}' is a send endpoint — use send(), not request()`);
        return;
      }
      if (name === 'acme.shipworks.getWeight') {
        // A value-returning plugin request: the typed reply payload is the whole
        // answer — there is no envelope type to inspect any more.
        setTimeout(() => ok({ weight: 42.5 }), 10);
        return;
      }
      if (isPluginEndpoint(name)) {
        // The documented minimum RegisterRequest handler behind a schema `action`
        // button: a payload with a `message` the settings view toasts.
        setTimeout(() => ok({ message: 'Done (mock)' }), 400);
        return;
      }
      fail('unknown-endpoint', 'no such endpoint');
      return;
    }

    switch (name) {
      case 'settings.set': {
        const allowed = writableMod(str(p, 'mod'));
        if (allowed === null) {
          fail('forbidden', "a view may only write its own mod's settings");
          break;
        }
        const key = str(p, 'key');
        const mod = mods.find((m) => m.id === allowed);
        const setting = mod ? findSetting(mod, key) : null;
        if (!('value' in p)) {
          fail('invalid-value', 'missing value field');
          break;
        }
        if (!mod || !setting) {
          fail('unknown-setting', 'unknown mod or setting');
          break;
        }
        const v = normalizeValue(setting, p['value']);
        if (v === undefined) {
          // A failed set REJECTS with its code. 1.x resolved an ack the caller had
          // to remember to inspect, so forgetting read as success.
          fail('invalid-value', 'the value was refused');
          break;
        }
        mod.values[key] = v;
        persist(mod);
        // Native order: the store commits and fans out `settings.changed` before
        // the request settles.
        pushChanged(allowed, key, v);
        pushPersisted(allowed);
        // `value` is the post-clamp COMMITTED value, so the caller can tell
        // clamped from accepted without a re-read.
        ok({ mod: allowed, key, value: v });
        break;
      }

      case 'settings.reset': {
        const allowed = writableMod(str(p, 'mod'));
        if (allowed === null) {
          fail('forbidden', "a view may only reset its own mod's settings");
          break;
        }
        const key = str(p, 'key');
        const mod = mods.find((m) => m.id === allowed);
        if (!mod) {
          fail('unknown-setting', 'unknown mod or setting');
          break;
        }
        // Native parity: no per-key settings.changed fan-out — the authoritative
        // `osfui/settings` republish below re-syncs every view.
        eachSetting(mod.schema, (s) => {
          if (!key || s.key === key) mod.values[s.key] = defaultFor(s) as SettingValue;
        });
        persist(mod);
        pushPersisted(allowed);
        // The reply says only "the reset happened"; the registry arrives the same
        // way it reaches everyone else.
        ok({});
        publishSettings();
        break;
      }

      case 'settings.captureKey': {
        // The REQUEST settles in machine time — armed, or a typed refusal — and
        // the human-time outcome arrives later as the `settings.captured` event.
        const allowed = writableMod(str(p, 'mod'));
        if (allowed === null) {
          fail('forbidden', "a view may only rebind its own mod's keys");
          break;
        }
        if (capture) {
          fail('capture-busy', 'a key capture is already in progress');
          break;
        }
        const key = str(p, 'key');
        const setting = findSetting(mods.find((m) => m.id === allowed), key);
        if (!setting || setting.type !== 'key') {
          fail('not-rebindable', 'only a key-typed setting can be rebound');
          break;
        }
        armCapture(allowed, key);
        ok({ armed: true, mod: allowed, key });
        break;
      }

      case 'menu.open': {
        const id = targetView();
        const page = HARNESS_PAGES[id];
        if (page) {
          // Real shipped view — hand off to its harness location, after a brief
          // delay like the in-game single-menu swap. The reply means "accepted and
          // queued", which is all the caller can act on.
          log('info', `menu.open ${id} → ${page}`);
          ok({});
          setTimeout(() => {
            location.href = page;
          }, 450);
        } else if (views.some((v) => v.id === id)) {
          // Fictional view — mark it open/focused and republish, which clears the
          // launch overlay (mirrors the runtime's reconcile).
          ok({});
          setTimeout(() => {
            for (const v of views) {
              if (v.kind === 'menu') {
                v.focused = v.id === id;
                v.open = v.open || v.id === id;
              }
            }
            publishViews();
          }, 400);
        } else {
          fail('unknown-view', 'view was not discovered');
        }
        break;
      }

      case 'menu.close': {
        const id = targetView();
        const v = views.find((x) => x.id === id);
        if (!v) {
		  fail('unknown-view', 'not a discovered view');
          break;
        }
        v.open = false;
        v.focused = false;
        ok({});
        setTimeout(() => publishViews(), 150); // async reconcile, like native
        break;
      }

      case 'setViewHidden':
        // Per-view hidden state has no field in the views catalog, so there is
        // nothing to reconcile — the reply is the whole observable behaviour.
        log('info', `setViewHidden ${targetView()} -> ${p['hidden'] === true}`);
        ok({});
        break;

      case 'osfui.setViewAutoStart': {
        // Startup policy is player intent: only the built-in Mod Settings view may
        // change it — the same exact-id gate the Mod Settings-only platform
        // requests use.
        if (selfView !== 'osfui/settings') {
          fail('forbidden', "view auto-start is set from OSF UI's built-in settings view");
          break;
        }
        const v = views.find((x) => x.id === str(p, 'view'));
        if (typeof p['enabled'] !== 'boolean' || !str(p, 'view')) {
          fail('invalid-payload', 'expected { view: string, enabled: boolean }');
          break;
        }
        if (!v) {
          fail('unknown-view', 'not a discovered view');
          break;
        }
        if (!v.autoStartMutable) {
          fail('not-configurable', 'auto-start is settable only for catalog-visible HUDs');
          break;
        }
        // The choice is next-launch policy, so open state never changes here.
        v.autoStart = p['enabled'] === true;
        ok({});
        setTimeout(() => publishViews(), 150); // async rebroadcast, like native
        break;
      }

      case 'ping':
        ok({});
        break;

      case 'game.get':
        // Nested per-provider: future providers are siblings of `calendar`. Fixed
        // sample date, enough to render a HUD clock.
        ok({
          calendar: {
            available: true,
            day: 12,
            month: 7,
            year: 2330,
            hour: 14.52,
            daysPassed: 87.3,
          },
        });
        break;

      case 'osfui.openLogFolder':
        // Payload-free and fixed-target in game; there is nothing to open from a
        // browser, so the harness just proves the request was fired.
        notify('osfui.openLogFolder — fired (opens the SFSE log folder in game)');
        ok({});
        break;

      case 'osfui.openModPage':
        notify('osfui.openModPage — fired (opens the mod page in game)');
        ok({});
        break;

      case 'papyrus.request':
        // Native defers to the mod's script listener; a harness has no Papyrus VM,
        // so it answers exactly as a game with no listener registered does.
        fail('papyrus-unavailable', 'no Papyrus request listener is available (no VM in the harness)');
        break;

      default:
        fail('unknown-endpoint', 'no such endpoint');
        break;
    }
  }

  /**
   * The inbound envelope check, mirroring MessageBridge::HandleWebMessage: routing
   * metadata sits BESIDE the payload, so no payload field can override it. Every
   * refusal is surfaced rather than dropped — except unparseable text, which
   * carries no id and no name to report against.
   */
  function receive(json: string): void {
    let m: { kind?: unknown; name?: unknown; id?: unknown; payload?: unknown };
    try {
      m = JSON.parse(json) as typeof m;
    } catch {
      log('←web', 'malformed message — dropped');
      return;
    }
    if (!m || typeof m !== 'object' || Array.isArray(m)) {
      log('←web', 'malformed message — dropped');
      return;
    }

    const kind = typeof m.kind === 'string' ? m.kind : '';
    const name = typeof m.name === 'string' ? m.name.slice(0, MAX_ECHOED_NAME_LENGTH) : '';
    log('←web', `${kind || '(no kind)'} ${name || '(no name)'}`);

    if (kind !== 'send' && kind !== 'request') {
      reportProtocolFault('invalid-request', 'kind must be "send" or "request"', {
        kind: String(m.kind).slice(0, MAX_ECHOED_NAME_LENGTH),
        name,
      });
      return;
    }
    if (!name) {
      reportProtocolFault('invalid-request', 'a message needs a non-empty endpoint name', { kind });
      return;
    }
    let payload: CommandPayload = {};
    if (m.payload !== undefined && m.payload !== null) {
      if (typeof m.payload !== 'object' || Array.isArray(m.payload)) {
        reportProtocolFault('invalid-request', 'payload must be an object', { kind, name });
        return;
      }
      payload = m.payload as CommandPayload;
    }
    const hasId = m.id !== undefined && m.id !== null;

    if (kind === 'send') {
      // `id` is forbidden on a send: a caller that supplied one expects a
      // settlement it will never get.
      if (hasId) {
        reportProtocolFault('invalid-request', 'send messages carry no id — use a request', { name });
        return;
      }
      dispatchSend(name, payload);
      return;
    }
    if (!hasId || typeof m.id !== 'string' || !m.id || m.id.length > MAX_REQUEST_ID_LENGTH) {
      // Not demoted to fire-and-forget the way 1.x demoted a bad requestId: silent
      // demotion turns a client bug into a request that never settles.
      reportProtocolFault(
        'invalid-request',
        `request id must be 1-${MAX_REQUEST_ID_LENGTH} characters`,
        { name },
      );
      return;
    }
    dispatchRequest(name, m.id, payload);
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
   * The OSF UI release version, read out of src/Core/Version.h so the harness
   * badge shows what the DLL would report. Best-effort: an unreachable file keeps the
   * "-mock" marker so a fake version is not mistaken for a real one. The greeting
   * waits on it, because `ready.version` is the reference point every advisory
   * `targetVersion` is compared against.
   */
  const osfuiReleaseVersion: Promise<string> = (async () => {
    const text = await loadOnly(VERSION_HEADER, 'src/Core/Version.h');
    if (text === null) return FALLBACK_OSFUI_RELEASE_VERSION;
    const m = /kOsfuiReleaseVersion\s*=\s*"([^"]+)"/.exec(text);
    if (!m || !m[1]) {
      console.warn(
        '[mock] Version.h has no kOsfuiReleaseVersion literal — using '
          + FALLBACK_OSFUI_RELEASE_VERSION,
      );
      return FALLBACK_OSFUI_RELEASE_VERSION;
    }
    return m[1];
  })();

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
      publishSettings();
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
      browserWindow.addEventListener(ev, stop, false);
    }
    browserWindow.addEventListener(
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
              // applyLocale re-merges catalogs and re-publishes both registries,
              // which covers plain schema drops too. A dropped catalog activates
              // its locale when none is selected.
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
      raise('ui.visibility', { visible: !!v, reason: 'overlay' });
    },
    locale: localeApi,
    hotkey(mod?: string, key?: string) {
      const target = mod && key ? { mod, key } : firstKeySetting();
      if (!target) {
        console.warn('[mock] no type:"key" setting in the registry — nothing to fire a hotkey for.');
        return false;
      }
      raise('ui.hotkey', { mod: target.mod, key: target.key });
      return true;
    },
    gamepad(button: 'LB' | 'RB') {
      const id = PAD_BUTTONS[button];
      const down: UiGamepadPayload = { kind: 'button', button: { id, down: true } };
      const up: UiGamepadPayload = { kind: 'button', button: { id, down: false } };
      raise('ui.gamepad', down);
      // The release matters: @lib/lifecycle's padButtonEdge reports a down edge
      // once per press and needs the up to re-arm, so a down-only injector would
      // fire exactly once per page load.
      setTimeout(() => raise('ui.gamepad', up), 0);
    },
    captureArmed: () => capture !== null,
    cancelCapture() {
      if (!capture) return false;
      finishCapture('', true);
      return true;
    },
    setSelfView(id: string) {
      selfView = id;
      // The i18n domain and the handoff key both key off the hosted view, so a
      // greeted document re-reads them rather than keeping the old view's.
      publishI18n();
      publishHandoff();
    },
    health: setHealth,
    healthScenario: () => healthScenario,
    greeted: () => greeted,
    loaded: () => initial,
  };

  // Install

  // Must happen before the shared kit loads: it decides `available` from
  // `typeof g.postMessage === "function"` and then takes ownership of onMessage.
  // Decorating rather than replacing keeps whatever the kit already put here if the
  // order ever gets swapped by accident.
  // Cast: Window.osfui is typed as the full injected bridge (postMessage +
  // onMessage both required) because in game it only ever exists fully formed; the
  // mock is what makes it exist, so it starts empty.
  const w = browserWindow as unknown as { osfui?: Record<string, unknown> };
  if (!w.osfui) w.osfui = {};
  const g = w.osfui;
  // Dispatch a macrotask late, so the mock crosses the same asynchronous boundary
  // the real bridge does: a view that sends from inside a state handler can never
  // re-enter its own delivery, and message order is still FIFO.
  g['postMessage'] = (json: string) => {
    setTimeout(() => receive(String(json)), 0);
  };
  if (!('onMessage' in g)) g['onMessage'] = null;
  g['_mock'] = api;

  // Seed immediately so a greeting that lands before the async sources resolve
  // still replays a registry, then upgrade once the real schemas arrive.
  FALLBACK_SCHEMAS.forEach(upsert);
  if (opts.drop !== false) wireDrop();
  const initial: Promise<void> = Promise.all([
    osfuiReleaseVersion,
    opts.autoLoad === false ? Promise.resolve() : loadSources(),
  ]).then(() => undefined);

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
