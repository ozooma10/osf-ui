// Injected head-first into every served view page as a CLASSIC script, so
// window.osfui.postMessage exists before the shared kit (also classic) decides
// whether a bridge is present. The page's own meta rides an inline script
// (window.__OSFUI_HARNESS_META__) injected just above this tag, so this file
// is fully static and one server route serves every view page.
//
// This file owns only the plumbing: the queues in both directions and the
// primitives the mock runtime needs. The runtime itself is an ES module
// (mock-runtime.js) pulled in by mock-loader.js — deferred, so any command
// the view sends before the mock settles waits in `outbound` and is answered
// once the runtime (or a full-takeover mock) is live. Nothing is dropped and
// nothing races, whichever script flavor the view entry uses.

(() => {
  'use strict';
  const SOURCE = 'osfui-harness';
  const META = window.__OSFUI_HARNESS_META__ || {};
  const inbound = [];   // native -> web, waiting for the kit's onMessage
  const outbound = [];  // web -> native, waiting for the mock to settle
  let handler = null;   // (text) => void, installed by the mock runtime

  const report = (direction, message, level = '') => {
    parent.postMessage({ source: SOURCE, kind: 'traffic', direction, message, level }, location.origin);
  };
  const deliver = (message) => {
    const text = JSON.stringify(message);
    const receive = window.osfui && window.osfui.onMessage;
    if (typeof receive === 'function') {
      receive(text);
    } else {
      inbound.push(text);
    }
  };
  const flush = () => {
    const receive = window.osfui && window.osfui.onMessage;
    if (typeof receive !== 'function') return false;
    while (inbound.length) receive(inbound.shift());
    return true;
  };
  const postMessage = (text) => {
    if (handler) handler(text);
    else outbound.push(String(text));
  };

  if (META.nativeBridge) {
    window.osfui = window.osfui || {};
    window.osfui.postMessage = postMessage;
  }
  // Vite's development client needs WebSocket for HMR. Network egress remains
  // constrained by the harness CSP and is checked again by the check command.
  // Note these run before any mock module: mocks cannot use workers either.
  for (const name of ['RTCPeerConnection', 'webkitRTCPeerConnection',
    'WebTransport', 'Worker', 'SharedWorker']) {
    try { Object.defineProperty(window, name, { value: undefined, writable: false, configurable: false }); }
    catch {}
  }
  window.addEventListener('message', (event) => {
    if (event.origin !== location.origin || !event.data || event.data.source !== SOURCE) return;
    if (event.data.kind === 'deliver') deliver(event.data.message);
  });

  // The contract with mock-loader.js / mock-runtime.js.
  window.__osfuiHarness = {
    meta: META,
    source: SOURCE,
    report,
    deliver,
    flush,
    /** The queuing stub — runtimes compare against it to detect a takeover. */
    bridgeEntry: postMessage,
    /** Install the command handler and drain everything the view already sent. */
    setHandler(next) {
      handler = next;
      while (outbound.length && handler) handler(outbound.shift());
    },
    /** Tell the shell the page is up (drives its status line). */
    ready() {
      parent.postMessage({ source: SOURCE, kind: 'ready' }, location.origin);
    },
    /** Mock health for the shell status line / traffic panel. */
    status(ok, message) {
      parent.postMessage({ source: SOURCE, kind: 'mock-status', ok, message }, location.origin);
    },
  };
})();
