// The mock runtime, loaded into the view iframe by mock-loader.js.
//
// Two tiers, matching @osfui/cli's declared surface:
//   - simple: `export default defineMock({state, locale, locales, requests,
//     scenarios})` — served by the scenario engine below. `requests` values
//     may be plain JSON, {$type, payload}, or (async) functions of the
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

/** Commands every host answers; a mock does not need to declare them. */
const BUILT_IN = new Set([
  'close', 'menu.open', 'hud.show', 'hud.hide', 'view.ready',
  'ui.action', 'osfui.handleBack', 'osfui.gamepadRaw', 'log',
]);

/**
 * The scenario engine: answers one ui.command against a resolved scenario.
 * `io.reply(type, payload)` echoes the command's requestId; `io.report`
 * mirrors the shell traffic log. Pure aside from what io provides.
 */
export function createScenarioHandler(scenario, meta) {
  const respond = async (key, payload) => {
    const value = scenario.requests[key];
    if (value === undefined) return null;
    const result = typeof value === 'function' ? await value(payload) : value;
    if (result && typeof result === 'object' && typeof result.$type === 'string') {
      return { type: result.$type, payload: result.payload ?? {} };
    }
    return { type: 'mock.result', payload: result };
  };
  return async (command, payload, requestId, io) => {
    if (command === 'i18n.get') {
      const locale = scenario.locale || 'en';
      io.reply('i18n.data', {
        mod: payload.mod || meta.modId,
        locale,
        strings: catalogFor(scenario, locale),
      });
      return true;
    }
    if (command === 'ui.papyrusRequest') {
      const response = await respond('papyrus.' + String(payload.request || ''), payload);
      if (response) {
        io.reply('papyrus.result', { value: response.payload });
      } else {
        io.reply('ui.error', {
          code: 'mock-unhandled', command,
          message: 'No mock response for Papyrus request "' + payload.request + '".',
        });
      }
      return true;
    }
    const response = await respond(command, payload);
    if (response) {
      io.reply(response.type, response.payload);
      return true;
    }
    if (BUILT_IN.has(command)) {
      io.reply('ui.result', { ok: true, command, message: 'Handled by browser harness' });
      return true;
    }
    const error = {
      code: 'mock-unhandled',
      command,
      message: 'No mock response is configured for "' + command + '".',
    };
    if (requestId) io.reply('ui.error', error);
    else io.report('in', { type: 'ui.error', payload: error }, 'warn');
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

  const reply = (requestId) => (type, payload) => {
    if (!requestId) return;
    const message = { type, requestId, payload };
    harness.report('in', message);
    queueMicrotask(() => harness.deliver(message));
  };
  const io = { report: harness.report };
  const engine = createScenarioHandler(scenario, meta);

  const handler = async (text) => {
    let message;
    try { message = JSON.parse(text); } catch {
      harness.report('out', text, 'warn');
      return;
    }
    harness.report('out', message);
    if (!message || message.type !== 'ui.command') return;
    const payload = message.payload || {};
    const command = String(payload.command || '');
    const respond = reply(message.requestId);
    for (const entry of chain) {
      try {
        if (await entry(command, payload, respond, message.requestId || '') === true) return;
      } catch (cause) {
        const detail = { code: 'mock-error', command, message: String(cause && cause.message || cause) };
        harness.report('in', 'Mock handler threw for "' + command + '": ' + (cause && cause.stack || cause), 'warn');
        respond('ui.error', detail);
        return;
      }
    }
    try {
      await engine(command, payload, message.requestId || '', { ...io, reply: respond });
    } catch (cause) {
      harness.report('in', 'Mock request for "' + command + '" threw: ' + (cause && cause.stack || cause), 'warn');
      respond('ui.error', { code: 'mock-error', command, message: String(cause && cause.message || cause) });
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

  // osfui.t wrapping. Wraps compose in registration order over the kit's
  // original t; pseudo mode is itself a wrap (pseudoize every resolution, so
  // inline t()/data-i18n strings pseudoize too, not just catalog lookups).
  // The kit loads after this module but decorates the same window.osfui, so
  // wraps apply lazily once t exists.
  const wraps = [];
  let baseT = null;
  const pseudoWrap = (t) => (address, english, vars) => String(pseudoize(t(address, english, vars)));
  const applyWraps = () => {
    const helper = window.osfui;
    if (!helper || typeof helper.t !== 'function') return;
    if (baseT === null) baseT = helper.t.bind(helper);
    let t = baseT;
    for (const wrap of wraps) t = wrap(t);
    helper.t = t;
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
              ctx.send({
                type: 'i18n.data',
                payload: { mod: meta.modId, locale, strings: catalogFor(scenario, locale) },
              });
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
  const greeting = {
    type: 'runtime.ready',
    payload: {
      game: 'Starfield', plugin: 'OSF UI Harness',
      version: meta.version, bridgeVersion: meta.bridgeVersion,
    },
  };
  harness.report('in', greeting);
  harness.deliver(greeting);
  for (const [key, value] of Object.entries(scenario.state)) {
    const message = { type: 'data.state', payload: { mod: meta.modId, key, value } };
    harness.report('in', message);
    harness.deliver(message);
  }
  // Locale switches pushed by the shell.
  window.addEventListener('message', (event) => {
    if (event.origin !== location.origin || !event.data || event.data.source !== harness.source) return;
    if (event.data.kind !== 'control' || event.data.action !== 'locale') return;
    const locale = String(event.data.locale || 'en');
    scenario.locale = locale;
    setPseudoWrap(locale === 'pseudo');
    const message = {
      type: 'i18n.data',
      payload: { mod: meta.modId, locale, strings: catalogFor(scenario, locale) },
    };
    harness.report('in', message);
    harness.deliver(message);
  });
  harness.ready();
  harness.setHandler(handler);
}
