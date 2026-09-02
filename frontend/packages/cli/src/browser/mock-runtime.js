// Protocol-aware browser mock for the singular OSF UI JavaScript surface.
// The pure endpoint engine is kept DOM-free so it can be tested under Node.

import { applyPatch, normalizeTools } from './tools-model.js';

export function resolveScenario(mock, name) {
  const base = mock && typeof mock === 'object' && !Array.isArray(mock) ? mock : {};
  const selected = name && base.scenarios && typeof base.scenarios === 'object'
    ? base.scenarios[name]
    : null;
  const overlay = selected && typeof selected === 'object' ? selected : {};
  return {
    state: { ...(base.state || {}), ...(overlay.state || {}) },
    requests: { ...(base.requests || {}), ...(overlay.requests || {}) },
    scenarioNames: base.scenarios && typeof base.scenarios === 'object'
      ? Object.keys(base.scenarios)
      : [],
    scenario: selected ? name : '',
  };
}

export const PLATFORM_SENDS = new Set([
  'osfui.hello',
  'close',
  'setVisible',
  'papyrus.call',
  'osfui.gamepadMode',
  'osfui.gamepadRaw',
  'osfui.handleBack',
  'osfui.relativePointer',
]);

export const PLATFORM_REQUESTS = new Set([
  'menu.open',
  'menu.close',
  'setViewHidden',
  'ping',
]);

const PLATFORM_SCRIPTS = new Set(['osfui', 'osfui_settings', 'osfui_view']);

function answerPlatformRequest(_name, _payload, _meta, io) {
  io.resolve({});
}

function ownQualifiedName(name, meta) {
  const prefix = `${meta.modId}.`;
  return typeof name === 'string' && name.startsWith(prefix) ? name.slice(prefix.length) : '';
}

function configuredRequest(scenario, name, meta) {
  if (Object.hasOwn(scenario.requests, name)) return { found: true, value: scenario.requests[name] };
  const local = ownQualifiedName(name, meta);
  if (local && Object.hasOwn(scenario.requests, local)) {
    return { found: true, value: scenario.requests[local] };
  }
  if (!name.includes('.')) {
    const qualified = `${meta.modId}.${name}`;
    if (Object.hasOwn(scenario.requests, qualified)) {
      return { found: true, value: scenario.requests[qualified] };
    }
  }
  return { found: false };
}

export function createEndpointHandler(scenario, meta) {
  return async (kind, name, payload, io) => {
    if (kind === 'send' && name === 'papyrus.call' &&
        PLATFORM_SCRIPTS.has(String(payload.script || '').toLowerCase())) {
      io.surface('forbidden',
        "papyrus.call cannot target OSF UI's own scripts — use the public bridge APIs");
      return true;
    }

    const configured = configuredRequest(scenario, name, meta);
    if (configured.found) {
      if (kind !== 'request') {
        io.surface('wrong-endpoint-kind', `"${name}" is configured as a request endpoint`);
        return true;
      }
      const result = typeof configured.value === 'function'
        ? await configured.value(payload)
        : configured.value;
      io.resolve(result);
      return true;
    }

    if (kind === 'send') {
      if (PLATFORM_REQUESTS.has(name)) {
        io.surface('wrong-endpoint-kind', `"${name}" is a request endpoint — use request(), not send()`);
      } else if (!PLATFORM_SENDS.has(name)) {
        io.surface('unknown-endpoint', `No mock handles the send "${name}".`);
      }
      return true;
    }

    if (PLATFORM_REQUESTS.has(name)) {
      answerPlatformRequest(name, payload, meta, io);
      return true;
    }
    if (PLATFORM_SENDS.has(name)) {
      io.reject('wrong-endpoint-kind', `"${name}" is a send endpoint — use send(), not request()`);
      return true;
    }
    io.reject('mock-unhandled', `No mock response is configured for "${name}".`);
    return true;
  };
}

// Kept as a source-compatible import name for mocks written against the first
// protocol-aware harness; the public context itself is now onEndpoint only.
export const createScenarioHandler = createEndpointHandler;

function safeStorage() {
  try {
    const probe = '__osfui_probe__';
    window.localStorage.setItem(probe, '1');
    window.localStorage.removeItem(probe);
    return window.localStorage;
  } catch {
    return null;
  }
}

function notify(text) {
  const toast = document.createElement('div');
  toast.textContent = String(text);
  toast.style.cssText = 'position:fixed;right:14px;bottom:14px;z-index:2147483647;' +
    'padding:8px 12px;font:13px/1.4 system-ui,sans-serif;color:#e8f2f6;' +
    'background:#111d24;border:1px solid #37505c;pointer-events:none;';
  (document.body || document.documentElement).append(toast);
  setTimeout(() => toast.remove(), 2600);
}

function waitForKit(harness) {
  if (harness.flush()) return Promise.resolve(true);
  return new Promise((resolve) => {
    const timer = setInterval(() => {
      if (!harness.flush()) return;
      clearInterval(timer);
      clearTimeout(giveUp);
      resolve(true);
    }, 0);
    const giveUp = setTimeout(() => {
      clearInterval(timer);
      resolve(false);
    }, 5000);
  });
}

export async function installMock(harness, mod, loadError = null) {
  const meta = harness.meta;
  let mockError = loadError;
  const params = new URLSearchParams(location.search);
  const scenario = resolveScenario(mod?.default, params.get('scenario'));
  const chain = [];

  const emit = (message) => {
    harness.report('in', message);
    queueMicrotask(() => harness.deliver(message));
  };
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
    for (const [key, value] of Object.entries(scenario.state)) {
      emit({ kind: 'state', mod: meta.modId, key, value });
    }
    if (mockError) {
      emit({
        kind: 'event',
        name: 'osfui.debug.error',
        payload: {
          code: 'mock-load-error',
          message: String(mockError?.message || mockError),
          detail: { view: meta.qualifiedId || '' },
        },
      });
    }
  };

  const settlements = (id) => {
    let settled = false;
    const once = (message) => {
      if (settled || !id) return;
      settled = true;
      emit(message);
    };
    return {
      resolve(...values) {
        once({ kind: 'reply', id, payload: values.length ? values[0] : {} });
      },
      reject(code, message) {
        once({ kind: 'error', id, payload: { code, message: message || '' } });
      },
      get settled() { return settled; },
    };
  };
  const surface = (code, message) => emit({
    kind: 'event',
    name: 'osfui.debug.error',
    payload: { code, message, detail: { view: meta.qualifiedId || '' } },
  });
  const engine = createEndpointHandler(scenario, meta);

  const handle = async (text) => {
    let message;
    try {
      message = JSON.parse(text);
    } catch {
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
      harness.report('in', `Request "${name}" arrived without an id — dropping.`, 'warn');
      return;
    }
    if (kind === 'send' && name === 'osfui.hello') {
      greet();
      return;
    }

    const settlement = settlements(id);
    const io = {
      report: harness.report,
      resolve: settlement.resolve,
      reject: settlement.reject,
      surface,
    };
    for (const endpoint of chain) {
      try {
        const handled = await endpoint(kind, name, payload, io);
        if (settlement.settled) return;
        if (handled === true) {
          if (kind === 'request') {
            settlement.reject('mock-unsettled', `Mock handled "${name}" without resolving or rejecting it.`);
          }
          return;
        }
      } catch (error) {
        harness.report('in', `Mock endpoint threw for "${name}": ${error?.stack || error}`, 'warn');
        if (kind === 'request') settlement.reject('mock-error', String(error?.message || error));
        return;
      }
    }
    try {
      await engine(kind, name, payload, io);
    } catch (error) {
      harness.report('in', `Mock engine threw for "${name}": ${error?.stack || error}`, 'warn');
      if (kind === 'request') settlement.reject('mock-error', String(error?.message || error));
    }
  };

  let tools = [];
  let onInvoke = null;
  const postTools = () => parent.postMessage(
    { source: harness.source, kind: 'tools', tools }, location.origin,
  );
  window.addEventListener('message', (event) => {
    if (event.origin !== location.origin || event.data?.source !== harness.source ||
        event.data.kind !== 'tool-invoke') return;
    try {
      onInvoke?.(event.data.id, event.data.value);
    } catch (error) {
      harness.report('in', `Tool "${event.data.id}" handler threw: ${error?.stack || error}`, 'warn');
    }
  });

  const ctx = {
    meta,
    params,
    storage: safeStorage(),
    scenario,
    send: emit,
    onEndpoint(endpoint) {
      if (typeof endpoint !== 'function') throw new TypeError('onEndpoint requires a function');
      chain.push(endpoint);
    },
    registerTools(raw, invoke) {
      const normalized = normalizeTools(raw);
      tools = normalized.tools;
      onInvoke = typeof invoke === 'function' ? invoke : null;
      for (const id of normalized.dropped) {
        harness.report('in', `Dropped invalid harness tool "${id}".`, 'warn');
      }
      postTools();
    },
    updateTool(id, patch) {
      tools = applyPatch(tools, id, patch);
      parent.postMessage({ source: harness.source, kind: 'tool-state', id, patch }, location.origin);
    },
    notify,
  };

  if (typeof mod?.install === 'function' && !mockError) {
    try {
      await mod.install(ctx);
    } catch (error) {
      mockError = error;
      chain.length = 0;
      harness.report('in', `Mock install failed: ${error?.stack || error}`, 'warn');
    }
  }
  // Clear controls left in the persistent shell by the previous iframe even
  // when this mock registers none or failed during import/install.
  postTools();
  harness.setHandler(handle);
  const ready = await waitForKit(harness);
  const statusMessage = mockError
    ? String(mockError?.message || mockError)
    : (ready ? '' : 'shared helper did not install onMessage within 5 seconds');
  harness.status(ready && !mockError, statusMessage);
  harness.ready();
}
