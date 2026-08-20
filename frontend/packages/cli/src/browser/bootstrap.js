// Injected before the shared helper so its bridge probe sees postMessage.
// Traffic queues in both directions until the helper and mock runtime exist;
// reload and first-open therefore use the same page-initiated hello path.
(() => {
  'use strict';
  const SOURCE = 'osfui-harness';
  const meta = window.__OSFUI_HARNESS_META__ || {};
  const inbound = [];
  const outbound = [];
  let handler = null;

  const report = (direction, message, level = '') => {
    parent.postMessage({ source: SOURCE, kind: 'traffic', direction, message, level }, location.origin);
  };
  const deliver = (message) => {
    const text = JSON.stringify(message);
    const receive = window.osfui?.onMessage;
    if (typeof receive === 'function') receive(text);
    else inbound.push(text);
  };
  const flush = () => {
    const receive = window.osfui?.onMessage;
    if (typeof receive !== 'function') return false;
    while (inbound.length) receive(inbound.shift());
    return true;
  };
  const bridgeEntry = (text) => {
    if (handler) handler(String(text));
    else outbound.push(String(text));
  };

  if (meta.nativeBridge) {
    window.osfui = window.osfui || {};
    window.osfui.postMessage = bridgeEntry;
  }
  for (const name of [
    'RTCPeerConnection', 'webkitRTCPeerConnection', 'WebTransport',
    'Worker', 'SharedWorker',
  ]) {
    try {
      Object.defineProperty(window, name, {
        value: undefined, writable: false, configurable: false,
      });
    } catch {}
  }
  window.addEventListener('message', (event) => {
    if (event.origin !== location.origin || event.data?.source !== SOURCE) return;
    if (event.data.kind === 'deliver') deliver(event.data.message);
  });

  window.__osfuiHarness = {
    meta,
    source: SOURCE,
    report,
    deliver,
    flush,
    bridgeEntry,
    setHandler(next) {
      handler = next;
      while (handler && outbound.length) handler(outbound.shift());
    },
    ready() {
      parent.postMessage({ source: SOURCE, kind: 'ready' }, location.origin);
    },
    status(ok, message) {
      parent.postMessage({ source: SOURCE, kind: 'mock-status', ok, message }, location.origin);
    },
  };
})();
