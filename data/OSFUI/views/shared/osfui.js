// osfui.js — the OSF UI bridge helper (bridge protocol 0.5, api-freeze item 5).
//
// Load it like the shared stylesheet, BEFORE your view's own script:
//   <script src="../../shared/osfui.js"></script>
//
// It decorates the native-injected `window.osfui` object (creating a stub when
// no bridge is present, e.g. a plain browser) with a deliberately THIN surface
// — this file is part of the frozen contract, so it grows features about as
// often as the protocol does:
//
//   osfui.available()            -> bridge present (false = standalone preview)
//   osfui.ready                  -> Promise of the runtime.ready payload
//   osfui.has("type:flags")      -> capability test (false before ready)
//   osfui.send(command, fields)  -> fire-and-forget ui.command; returns false
//                                   when no bridge is present
//   osfui.request(command, fields, { timeoutMs }) -> Promise of the reply
//                                   MESSAGE ({ type, payload, requestId }).
//                                   Rejects (Error with .code) on ui.error, on
//                                   ui.result { ok:false }, on timeout
//                                   (default 10000 ms; 0 disables — e.g. a
//                                   key capture that waits on the user), and
//                                   immediately when no bridge is present.
//   osfui.on(type, fn)           -> subscribe to a native->web message type;
//                                   fn(payload, message); returns unsubscribe.
//
// The helper OWNS `osfui.onMessage` — with it loaded, never assign onMessage
// yourself; use osfui.on(). Replies that resolve a request() ALSO dispatch to
// on() subscribers (so one render path can consume settings.data no matter who
// asked); commands with no reply type of their own resolve with
// `ui.result { ok, command, code?, message? }`.

"use strict";

(function () {
  const g = (window.osfui = window.osfui || {});
  const listeners = new Map();  // type -> Set<fn>
  const pending = new Map();    // requestId -> { resolve, reject, timer }
  let seq = 0;

  let resolveReady;
  let readyPayload = null;
  g.ready = new Promise((r) => { resolveReady = r; });
  g.available = () => typeof g.postMessage === "function";
  g.has = (cap) =>
    !!(readyPayload && Array.isArray(readyPayload.capabilities) && readyPayload.capabilities.indexOf(cap) >= 0);

  g.send = (command, fields) => {
    if (!g.available()) return false;
    g.postMessage(JSON.stringify({ type: "ui.command", payload: Object.assign({ command }, fields || {}) }));
    return true;
  };

  g.request = (command, fields, opts) => {
    if (!g.available()) {
      const err = new Error("no bridge (standalone preview)");
      err.code = "no-bridge";
      return Promise.reject(err);
    }
    const requestId = "q" + (++seq);
    const timeoutMs = opts && "timeoutMs" in opts ? opts.timeoutMs : 10000;
    return new Promise((resolve, reject) => {
      let timer = 0;
      if (timeoutMs > 0) {
        timer = setTimeout(() => {
          pending.delete(requestId);
          const err = new Error(`"${command}" got no reply within ${timeoutMs}ms`);
          err.code = "timeout";
          reject(err);
        }, timeoutMs);
      }
      pending.set(requestId, { resolve, reject, timer });
      g.postMessage(JSON.stringify({ type: "ui.command", requestId, payload: Object.assign({ command }, fields || {}) }));
    });
  };

  g.on = (type, fn) => {
    let set = listeners.get(type);
    if (!set) listeners.set(type, (set = new Set()));
    set.add(fn);
    return () => set.delete(fn);
  };

  g.onMessage = (json) => {
    let message;
    try { message = JSON.parse(json); } catch { return; }
    if (!message || typeof message.type !== "string") return;
    if (message.type === "runtime.ready") {
      readyPayload = message.payload || {};
      resolveReady(readyPayload);
    }
    // Correlated reply: settle the request() promise...
    const rid = typeof message.requestId === "string" ? message.requestId : "";
    const req = rid && pending.get(rid);
    if (req) {
      pending.delete(rid);
      if (req.timer) clearTimeout(req.timer);
      const p = message.payload || {};
      if (message.type === "ui.error" || (message.type === "ui.result" && p.ok === false)) {
        const err = new Error(p.message || p.reason || p.code || "request failed");
        err.code = p.code || "";
        err.reply = message;
        req.reject(err);
      } else {
        req.resolve(message);
      }
    }
    // ...and ALWAYS dispatch to type subscribers (see the header note).
    const set = listeners.get(message.type);
    if (set) {
      for (const fn of [...set]) {
        try { fn(message.payload || {}, message); } catch (e) { console.error("osfui.on handler failed:", e); }
      }
    }
  };
})();
