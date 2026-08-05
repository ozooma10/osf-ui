// The mock runtime, loaded into the view iframe by mock-loader.js.
//
// Two tiers, matching @osfui/cli's declared surface:
//   - simple: `export default defineMock({state, locale, locales, requests,
//     scenarios})` — served by the scenario engine below. `requests` values
//     may be plain JSON, {$payload}, or (async) functions of the
//     command payload. `?scenario=<name>` (or ctx code) overlays a named
//     scenario onto the base fields.
//   - escape hatch: `export function install(ctx)` — gets the MockContext and
//     may register command handlers, push messages, or take over
//     window.osfui.postMessage wholesale (the repo's own mockbridge does).
//
// The pure pieces (scenario resolution, the command engine) take no DOM so
// they are unit-tested under node --test.

import { pseudoize, pseudoizeStrings } from './pseudo.js';
import { applyPatch, normalizeTools } from './tools-model.js';

/** The active-locale catalog; 'pseudo' pseudo-localizes the authored English. */
export function catalogFor(scenario, locale) {
  if (locale === 'pseudo') return pseudoizeStrings(scenario.locales.en || {});
  return scenario.locales[locale] || {};
}

/** Shallow-overlay a named scenario onto the base mock fields. */
export function resolveScenario(mock, name) {
  const base = mock && typeof mock === 'object' ? mock : {};
  const chosen = name && base.scenarios && typeof base.scenarios === 'object'
    ? base.scenarios[name]
    : null;
  const overlay = chosen && typeof chosen === 'object' ? chosen : {};
  return {
    state: { ...(base.state || {}), ...(overlay.state || {}) },
    locale: overlay.locale || base.locale || 'en',
    locales: { ...(base.locales || {}), ...(overlay.locales || {}) },
    requests: { ...(base.requests || {}), ...(overlay.requests || {}) },
    scenarioNames: base.scenarios && typeof base.scenarios === 'object'
      ? Object.keys(base.scenarios)
      : [],
    scenario: chosen ? name : '',
    $error: base.$error,
  };
}

/**
 * Endpoints every host answers, split by KIND — because the kind is what a
 * caller dispatches on, and a mock that got it wrong would let a view ship a
 * `send` to a request endpoint that only fails against the real runtime.
 * A mock does not need to declare any of these.
 */
export const PLATFORM_SENDS = new Set([
  'osfui.hello', 'close', 'setVisible', 'view.ready', 'log',
  'osfui.handleBack', 'osfui.gamepadRaw', 'osfui.handoffRetry',
  'papyrus.call', 'papyrus.send',
]);
export const PLATFORM_REQUESTS = new Set([
  'menu.open', 'menu.close', 'setViewHidden', 'ping', 'game.get',
  'settings.set', 'settings.reset', 'settings.captureKey',
  'osfui.openModPage', 'osfui.openLogFolder', 'osfui.setViewAutoStart',
  'papyrus.request',
]);

const PLATFORM_PRIVATE_REQUESTS = new Set([
  'osfui.setViewAutoStart',
]);

function ownSettings(payload, meta, io, verb) {
  const mod = typeof payload.mod === 'string' ? payload.mod : '';
  const key = typeof payload.key === 'string' ? payload.key : '';
  if (mod !== meta.modId && meta.qualifiedId !== 'osfui/settings') {
    io.reject('forbidden', `a view may only ${verb} its own mod's settings`);
    return null;
  }
  if (!mod || !key) {
    io.reject('invalid-request', `${verb} requires non-empty 'mod' and 'key' fields`);
    return null;
  }
  return { mod, key };
}

function answerPlatformRequest(name, payload, meta, io) {
  if (PLATFORM_PRIVATE_REQUESTS.has(name) && meta.qualifiedId !== 'osfui/settings') {
    io.reject('forbidden', `${name} is a platform action`);
    return;
  }
  switch (name) {
    case 'game.get':
      io.resolve({ calendar: { available: false } });
      return;
    case 'settings.set': {
      const target = ownSettings(payload, meta, io, 'write');
      if (!target) return;
      if (!Object.hasOwn(payload, 'value')) {
        io.reject('invalid-value', 'settings.set requires a value');
        return;
      }
      io.resolve({ ...target, value: payload.value });
      return;
    }
    case 'settings.reset': {
      const mod = typeof payload.mod === 'string' ? payload.mod : '';
      if (mod !== meta.modId && meta.qualifiedId !== 'osfui/settings') {
        io.reject('forbidden', "a view may only reset its own mod's settings");
        return;
      }
      if (!mod) {
        io.reject('invalid-request', "settings.reset requires a non-empty 'mod' field");
        return;
      }
      io.resolve({});
      return;
    }
    case 'settings.captureKey': {
      const target = ownSettings(payload, meta, io, 'rebind');
      if (!target) return;
      io.resolve({ armed: true, ...target });
      return;
    }
    default:
      io.resolve({});
  }
}

/**
 * The scenario engine: answers one inbound envelope against a resolved
 * scenario. `io.resolve(payload)` / `io.reject(code, message)` settle a
 * request; `io.report` mirrors the shell traffic log. Pure aside from io.
 */
export function createScenarioHandler(scenario, meta) {
  const respond = async (key, payload) => {
    const value = scenario.requests[key];
    if (value === undefined) return null;
    const result = typeof value === 'function' ? await value(payload) : value;
    // `$type` was how a 1.x mock chose the reply's message type. There is no
    // reply type any more — a reply is just a payload — so the wrapper only
    // still exists to let a mock nest its payload.
    if (result && typeof result === 'object' && typeof result.$payload !== 'undefined') {
      return { payload: result.$payload };
    }
    return { payload: result };
  };
  return async (kind, name, payload, io) => {
    if (kind === 'request' && name === 'papyrus.request') {
      const response = await respond('papyrus.' + String(payload.name || ''), payload);
      if (response) {
        io.resolve({ value: response.payload });
      } else {
        io.reject('mock-unhandled',
          'No mock response for Papyrus request "' + payload.name + '".');
      }
      return true;
    }
    if (kind === 'send' && name === 'papyrus.call' &&
        String(payload.script || '').toLowerCase() === 'osfui') {
      io.surface('forbidden',
        "papyrus.call cannot target OSF UI's own script — use the osfui.* endpoints");
      return true;
    }
    if (kind === 'send' && name === 'osfui.handoffRetry' &&
        meta.qualifiedId !== 'osfui/handoff') {
      io.surface('forbidden', 'osfui.handoffRetry is a platform action');
      return true;
    }
    const response = await respond(name, payload);
    if (response) {
      // A scenario answer to a `send` is a mock authoring mistake, not a
      // reply: a send has nothing to settle.
      if (kind === 'request') io.resolve(response.payload);
      else io.report('in', 'Scenario answers "' + name + '", but the view sent it one-way.', 'warn');
      return true;
    }
    if (kind === 'send') {
      if (!PLATFORM_SENDS.has(name)) {
        // Surfaced, not silent — the whole point of the 2.0 error routing.
        io.surface('unknown-endpoint', 'No mock handles the send "' + name + '".');
      }
      return true;
    }
    if (PLATFORM_REQUESTS.has(name)) {
      answerPlatformRequest(name, payload, meta, io);
      return true;
    }
    if (PLATFORM_SENDS.has(name)) {
      io.reject('wrong-endpoint-kind', '"' + name + '" is a send endpoint — use send(), not request().');
      return true;
    }
    io.reject('mock-unhandled', 'No mock response is configured for "' + name + '".');
    return true;
  };
}

function safeStorage() {
  try {
    const probe = '__osfui_probe__';
    window.localStorage.setItem(probe, '1');
    window.localStorage.removeItem(probe);
    return window.localStorage;
  } catch {
    return null; // private mode / quota — mocks fall back to memory-only
  }
}

/** Transient toast inside the view page for mocked commands whose real effect
 * is outside the browser. Inline-styled: the view page has no harness CSS. */
function notify(text) {
  const toast = document.createElement('div');
  toast.textContent = String(text);
  toast.style.cssText = 'position:fixed;right:14px;bottom:14px;z-index:2147483647;' +
    'padding:8px 12px;font:13px/1.4 system-ui,sans-serif;color:#e8f2f6;' +
    'background:#111d24;border:1px solid #37505c;pointer-events:none;';
  document.body.append(toast);
  setTimeout(() => toast.remove(), 2600);
}

const domReady = () => new Promise((settle) => {
  if (document.readyState !== 'loading') settle();
  else document.addEventListener('DOMContentLoaded', () => settle(), { once: true });
});

/** Poll until the shared kit's onMessage exists (or give up after 5s). */
const kitReady = (harness) => new Promise((settle) => {
  if (harness.flush()) { settle(true); return; }
  const timer = setInterval(() => {
    if (!harness.flush()) return;
    clearInterval(timer);
    clearTimeout(giveUp);
    settle(true);
  }, 0);
  const giveUp = setTimeout(() => {
    clearInterval(timer);
    settle(false);
  }, 5000);
});

export async function installMock(harness, mod, loadError) {
  const meta = harness.meta;
  const params = new URLSearchParams(location.search);
  const scenario = resolveScenario(mod?.default, params.get('scenario'));
  // ?locale= (forwarded by the shell) overrides the scenario's locale;
  // 'pseudo' is always valid — it derives from the authored English.
  if (params.get('locale')) scenario.locale = params.get('locale');
  const chain = [];

  const emit = (message) => {
    harness.report('in', message);
    queueMicrotask(() => harness.deliver(message));
  };

  /** The locale catalog as its state envelope — replayed and pushed alike. */
  const localeState = (locale) => ({
    kind: 'state',
    mod: 'osfui',
    key: 'i18n',
    value: { mod: meta.modId, locale, strings: catalogFor(scenario, locale) },
  });

  /**
   * Answer a page's `osfui.hello`: ready, then every current state value, then
   * events. Running on the page's greeting rather than on a timer is what
   * makes an F5 identical to a first open — the harness cannot deliver a
   * greeting the document missed, because the document asks for it.
   */
  const greet = () => {
    emit({
      kind: 'ready',
      payload: {
        game: 'Starfield',
        plugin: 'OSF UI Harness',
        version: meta.version,
        bridgeVersion: meta.bridgeVersion,
        view: meta.qualifiedId || '',
        mod: meta.modId || '',
      },
    });
    emit(localeState(scenario.locale || 'en'));
    for (const [key, value] of Object.entries(scenario.state)) {
      emit({ kind: 'state', mod: meta.modId, key, value });
    }
  };
  // Settlement is one-shot: a mock that answers twice must not deliver twice,
  // exactly as the real bridge ignores a late or duplicate reply.
  const settlers = (id) => {
    let settled = false;
    const once = (message) => {
      if (settled || !id) return;
      settled = true;
      emit(message);
    };
    return {
      resolve: (payload) => once({ kind: 'reply', id, payload: payload ?? {} }),
      reject: (code, message) => once({ kind: 'error', id, payload: { code, message: message || '' } }),
    };
  };
  // Host-detected misuse a `send` caller would otherwise never hear about.
  const surface = (view) => (code, message) =>
    emit({ kind: 'event', name: 'osfui.debug.error', payload: { code, message, detail: { view } } });
  const engine = createScenarioHandler(scenario, meta);

  const handler = async (text) => {
    let message;
    try { message = JSON.parse(text); } catch {
      harness.report('out', text, 'warn');
      return;
    }
    harness.report('out', message);
    if (!message || (message.kind !== 'send' && message.kind !== 'request')) return;
    const kind = message.kind;
    const name = String(message.name || '');
    const payload = message.payload || {};
    const id = typeof message.id === 'string' ? message.id : '';
    if (kind === 'request' && !id) {
      harness.report('in', 'Request "' + name + '" arrived without an id — dropping.', 'warn');
      return;
    }
    // The handshake is page-initiated and is the only boot path.
    if (kind === 'send' && name === 'osfui.hello') {
      greet();
      return;
    }
    const { resolve, reject } = settlers(id);
    const io = { report: harness.report, resolve, reject, surface: surface(meta.qualifiedId) };
    for (const entry of chain) {
      try {
        if (await entry(kind, name, payload, io) === true) return;
      } catch (cause) {
        harness.report('in', 'Mock handler threw for "' + name + '": ' + (cause && cause.stack || cause), 'warn');
        if (kind === 'request') reject('mock-error', String(cause && cause.message || cause));
        return;
      }
    }
    try {
      await engine(kind, name, payload, io);
    } catch (cause) {
      harness.report('in', 'Mock handler for "' + name + '" threw: ' + (cause && cause.stack || cause), 'warn');
      if (kind === 'request') reject('mock-error', String(cause && cause.message || cause));
    }
  };

  // Tool strip: dev controls rendered by the shell toolbar. The scenario
  // select is built in; mocks add their own via ctx.registerTools. Both ride
  // postMessage kinds 'tools' / 'tool-state' / 'tool-invoke' — dev plumbing,
  // never gated on nativeBridge.
  let builtinTools = [];
  let userTools = [];
  let userInvoke = null;
  const postTools = () => {
    parent.postMessage(
      { source: harness.source, kind: 'tools', tools: [...builtinTools, ...userTools] },
      location.origin,
    );
  };
  if (scenario.scenarioNames.length) {
    builtinTools = normalizeTools([{
      id: 'scenario',
      kind: 'select',
      label: 'Scenario',
      title: 'Overlay a named scenario from the mock onto its base fields (?scenario=<name>)',
      options: [{ value: '', label: '(base)' }, ...scenario.scenarioNames],
      value: scenario.scenario,
    }]).tools;
  }
  window.addEventListener('message', (event) => {
    if (event.origin !== location.origin || !event.data || event.data.source !== harness.source) return;
    if (event.data.kind === 'drop' && Array.isArray(event.data.files)) {
      takeDrop(event.data.files);
      return;
    }
    if (event.data.kind !== 'tool-invoke') return;
    const { id, value } = event.data;
    if (id === 'scenario' && builtinTools.some((tool) => tool.id === 'scenario')) {
      const url = new URL(location.href);
      if (value) url.searchParams.set('scenario', String(value));
      else url.searchParams.delete('scenario');
      location.href = url;
      return;
    }
    if (userInvoke) {
      try {
        userInvoke(id, value);
      } catch (cause) {
        harness.report('in', 'Tool "' + id + '" handler threw: ' + (cause && cause.stack || cause), 'warn');
      }
    }
  });

  // osfui.i18n.t wrapping. Wraps compose in registration order over the kit's
  // original t; pseudo mode is itself a wrap (pseudoize every resolution, so
  // inline t()/data-i18n strings pseudoize too, not just catalog lookups).
  // The kit loads after this module but decorates the same window.osfui, so
  // wraps apply lazily once t exists.
  const wraps = [];
  let baseT = null;
  const pseudoWrap = (t) => (address, english, vars) => String(pseudoize(t(address, english, vars)));
  const applyWraps = () => {
    const helper = window.osfui;
    if (!helper?.i18n || typeof helper.i18n.t !== 'function') return;
    if (baseT === null) baseT = helper.i18n.t.bind(helper.i18n);
    let t = baseT;
    for (const wrap of wraps) t = wrap(t);
    helper.i18n.t = t;
  };
  const setPseudoWrap = (on) => {
    const index = wraps.indexOf(pseudoWrap);
    if (on && index < 0) wraps.push(pseudoWrap);
    if (!on && index >= 0) wraps.splice(index, 1);
    applyWraps();
  };

  // Dropped files forwarded by the shell (kind 'drop'). <modId>_<locale>.json
  // catalogs merge into the scenario; everything else goes to ctx.onDrop.
  const dropHandlers = [];
  const CATALOG_NAME = /^(.+)_([A-Za-z][A-Za-z0-9-]{0,15})\.json$/;
  const takeDrop = (files) => {
    const rest = [];
    for (const file of files) {
      const match = CATALOG_NAME.exec(file.name || '');
      let merged = false;
      if (match) {
        try {
          const catalog = JSON.parse(file.text);
          if (catalog && typeof catalog === 'object' && !Array.isArray(catalog)) {
            const locale = match[2];
            scenario.locales[locale] = { ...(scenario.locales[locale] || {}), ...catalog };
            harness.report('in', 'Merged l10n catalog ' + file.name + ' (' + locale + ')');
            if ((scenario.locale || 'en') === locale) {
              ctx.send(localeState(locale));
            }
            merged = true;
          }
        } catch {}
      }
      if (!merged) rest.push(file);
    }
    if (!rest.length) return;
    if (!dropHandlers.length) {
      harness.report('in', 'Dropped file(s) not recognized as l10n catalogs and no ctx.onDrop handler: ' +
        rest.map((file) => file.name).join(', '), 'warn');
      return;
    }
    for (const handler of dropHandlers) {
      try { handler(rest); } catch (cause) {
        harness.report('in', 'Drop handler threw: ' + (cause && cause.stack || cause), 'warn');
      }
    }
  };

  const ctx = {
    meta,
    params,
    storage: safeStorage(),
    scenario: mod?.default ?? null,
    onDrop(handler) {
      if (typeof handler === 'function') dropHandlers.push(handler);
    },
    wrapT(wrap) {
      if (typeof wrap !== 'function') return;
      wraps.push(wrap);
      applyWraps();
    },
    registerTools(list, onInvoke) {
      const { tools, dropped } = normalizeTools(list);
      if (dropped.length) {
        harness.report('in', 'Dropped invalid tool spec(s): ' + dropped.join(', '), 'warn');
      }
      userTools = tools;
      userInvoke = typeof onInvoke === 'function' ? onInvoke : null;
      postTools();
    },
    updateTool(id, patch) {
      userTools = applyPatch(userTools, id, patch);
      builtinTools = applyPatch(builtinTools, id, patch);
      parent.postMessage(
        { source: harness.source, kind: 'tool-state', id, patch },
        location.origin,
      );
    },
    send(message) {
      if (!meta.nativeBridge) {
        harness.report('in', 'Bridge disabled by manifest.permissions.nativeBridge', 'warn');
        return;
      }
      harness.report('in', message);
      harness.deliver(message);
    },
    onCommand(entry) {
      if (typeof entry === 'function') chain.push(entry);
    },
    notify,
    log(message, level = 'info') {
      harness.report('in', message, level === 'warn' ? 'warn' : '');
    },
  };

  if (loadError) {
    harness.status(false, 'Mock module failed: ' + String(loadError && loadError.message || loadError));
    harness.report('in', 'Mock module failed: ' + String(loadError && loadError.stack || loadError), 'warn');
  } else if (meta.mockUrl) {
    harness.status(true, meta.mockName || 'mock loaded');
  }
  if (scenario.$error) harness.report('in', scenario.$error, 'warn');

  if (typeof mod?.install === 'function') {
    await mod.install(ctx);
    // A full-takeover mock replaces window.osfui.postMessage itself and owns
    // greeting and replies from here on; drain the backlog into it and stop.
    const current = window.osfui && window.osfui.postMessage;
    if (meta.nativeBridge && typeof current === 'function' && current !== harness.bridgeEntry) {
      postTools();
      harness.setHandler(current);
      harness.ready();
      return;
    }
  }
  // Always post (even empty): a reloaded iframe must clear a stale strip.
  postTools();

  await domReady();
  if (!meta.nativeBridge) {
    harness.ready();
    return;
  }
  const kit = await kitReady(harness);
  if (!kit) {
    harness.report('in', 'The shared kit never installed window.osfui.onMessage — is shared/osfui.js loaded?', 'warn');
  }
  // The kit exists now (or never will): apply t-wraps and the initial pseudo state.
  setPseudoWrap((scenario.locale || 'en') === 'pseudo');

  // Locale switches pushed by the shell. The catalog is a STATE key, so this is
  // the same message the greeting replays — one shape, one code path.
  window.addEventListener('message', (event) => {
    if (event.origin !== location.origin || !event.data || event.data.source !== harness.source) return;
    if (event.data.kind !== 'control' || event.data.action !== 'locale') return;
    const locale = String(event.data.locale || 'en');
    scenario.locale = locale;
    setPseudoWrap(locale === 'pseudo');
    emit(localeState(locale));
  });
  harness.ready();
  harness.setHandler(handler);
}
