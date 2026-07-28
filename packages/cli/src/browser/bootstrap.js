// Injected head-first into every served view page as a CLASSIC script, so
// window.osfui.postMessage exists before the shared kit (also classic) decides
// whether a bridge is present. The server prepends
// `const __OSFUI_HARNESS_META__ = {...}` when serving this file.

(() => {
  'use strict';
  const SOURCE = 'osfui-harness';
  const queued = [];
  let fixture = { state: {}, requests: {}, locales: {} };

  const report = (direction, message, level = '') => {
    parent.postMessage({ source: SOURCE, kind: 'traffic', direction, message, level }, location.origin);
  };
  const deliver = (message) => {
    const text = JSON.stringify(message);
    const receive = window.osfui && window.osfui.onMessage;
    if (typeof receive === 'function') {
      receive(text);
    } else {
      queued.push(text);
    }
  };
  const flush = () => {
    const receive = window.osfui && window.osfui.onMessage;
    if (typeof receive !== 'function') return false;
    while (queued.length) receive(queued.shift());
    return true;
  };
  const reply = (requestId, type, payload) => {
    if (!requestId) return;
    const message = { type, requestId, payload };
    report('in', message);
    queueMicrotask(() => deliver(message));
  };
  const commandResponse = (command) => {
    const value = fixture.requests && fixture.requests[command];
    if (value === undefined) return null;
    if (value && typeof value === 'object' && typeof value.$type === 'string') {
      return { type: value.$type, payload: value.payload ?? {} };
    }
    return { type: 'mock.result', payload: value };
  };
  const postMessage = (text) => {
    let message;
    try { message = JSON.parse(text); } catch {
      report('out', text, 'warn');
      return;
    }
    report('out', message);
    if (!message || message.type !== 'ui.command') return;
    const payload = message.payload || {};
    const command = String(payload.command || '');
    if (command === 'i18n.get') {
      const locale = fixture.locale || 'en';
      const strings = fixture.locales?.[locale] || {};
      reply(message.requestId, 'i18n.data', { mod: payload.mod || __OSFUI_HARNESS_META__.modId, locale, strings });
      return;
    }
    if (command === 'ui.papyrusRequest') {
      const response = commandResponse('papyrus.' + String(payload.request || ''));
      if (response) reply(message.requestId, 'papyrus.result', { value: response.payload });
      else reply(message.requestId, 'ui.error', {
        code: 'mock-unhandled', command, message: 'No osfui.mock.json response for Papyrus request "' + payload.request + '".'
      });
      return;
    }
    const response = commandResponse(command);
    if (response) {
      reply(message.requestId, response.type, response.payload);
      return;
    }
    const builtIn = new Set([
      'close', 'menu.open', 'hud.show', 'hud.hide', 'view.ready',
      'ui.action', 'osfui.handleBack', 'osfui.gamepadRaw', 'log'
    ]);
    if (builtIn.has(command)) {
      reply(message.requestId, 'ui.result', { ok: true, command, message: 'Handled by browser harness' });
      return;
    }
    const error = {
      code: 'mock-unhandled',
      command,
      message: 'No browser-harness response is configured for "' + command + '".'
    };
    if (message.requestId) reply(message.requestId, 'ui.error', error);
    else report('in', { type: 'ui.error', payload: error }, 'warn');
  };

  if (__OSFUI_HARNESS_META__.nativeBridge) {
    window.osfui = window.osfui || {};
    window.osfui.postMessage = postMessage;
  }
  // Vite's development client needs WebSocket for HMR. Network egress remains
  // constrained by the harness CSP and is checked again by the check command.
  for (const name of ['RTCPeerConnection', 'webkitRTCPeerConnection',
    'WebTransport', 'Worker', 'SharedWorker']) {
    try { Object.defineProperty(window, name, { value: undefined, writable: false, configurable: false }); }
    catch {}
  }
  window.addEventListener('message', (event) => {
    if (event.origin !== location.origin || !event.data || event.data.source !== SOURCE) return;
    if (event.data.kind === 'deliver') {
      deliver(event.data.message);
      return;
    }
    if (event.data.kind === 'control' && event.data.action === 'locale' &&
        __OSFUI_HARNESS_META__.nativeBridge) {
      const locale = String(event.data.locale || 'en');
      fixture.locale = locale;
      const message = {
        type: 'i18n.data',
        payload: {
          mod: __OSFUI_HARNESS_META__.modId,
          locale,
          strings: fixture.locales?.[locale] || {},
        },
      };
      report('in', message);
      deliver(message);
    }
  });
  window.addEventListener('DOMContentLoaded', async () => {
    try {
      fixture = await fetch('/__osfui/fixture.json', { cache: 'no-store' }).then((r) => r.json());
    } catch {}
    if (fixture.$error) report('in', fixture.$error, 'warn');
    if (!__OSFUI_HARNESS_META__.nativeBridge) {
      parent.postMessage({ source: SOURCE, kind: 'ready' }, location.origin);
      return;
    }
    const timer = setInterval(() => {
      if (!flush()) return;
      clearInterval(timer);
      const ready = {
        type: 'runtime.ready',
        payload: { game: 'Starfield', plugin: 'OSF UI Harness', version: __OSFUI_HARNESS_META__.version,
          bridgeVersion: __OSFUI_HARNESS_META__.bridgeVersion }
      };
      report('in', ready);
      deliver(ready);
      for (const [key, value] of Object.entries(fixture.state || {})) {
        const state = { type: 'data.state', payload: { mod: __OSFUI_HARNESS_META__.modId, key, value } };
        report('in', state);
        deliver(state);
      }
      parent.postMessage({ source: SOURCE, kind: 'ready' }, location.origin);
    }, 0);
    setTimeout(() => clearInterval(timer), 5000);
  });
})();
