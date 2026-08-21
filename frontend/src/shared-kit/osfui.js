// osfui.js — OSF UI bridge helper (mod API 2.0, bridge protocol 2.0).
//
// Load it like the shared stylesheet, BEFORE your view's own script:
//   <script src="https://osfui-assets.example/osfui.js"></script>
//
// Decorates the native-injected `window.osfui` (creating a stub when no bridge
// is present, e.g. a plain browser). The author API is deliberately only four
// operations, chosen by semantics:
//
//   osfui.send(name, payload?)            -> one-way with a named JSON payload.
//   osfui.send(name, arg, ...args)        -> Papyrus shorthand; posts
//                                            { args: [arg, ...args] }.
//                                            Returns "posted locally", never a
//                                            remote outcome. Wanting one means
//                                            it is a request.
//   osfui.request(name, payload?, opts?)  -> Promise of the reply PAYLOAD.
//   osfui.request(name, arg, ...args)      -> Papyrus shorthand; posts
//                                            { args: [arg, ...args] }.
//                                            Settles exactly once: payload,
//                                            typed error (err.code), or timeout
//                                            (default 10000 ms; 0 disables only
//                                            the client timer).
//   osfui.on(event, fn)                   -> one-shot happenings. Never replayed;
//                                            request replies never land here.
//                                            Own events use their local name;
//                                            qualified names still work.
//                                            Returns unsubscribe.
//   osfui.state.on(key, fn)               -> named values, latest-wins, complete
//                                            per key. Replays the current value
//                                            synchronously on subscribe and
//                                            again on every fresh document, so a
//                                            correct view needs ZERO lifecycle
//                                            code. Own state uses its local key;
//                                            qualified "<mod>/<key>" keys still
//                                            work.
//
//   osfui.state.get(key)                  -> latest value, or undefined.
//
// Reload is the common case. This helper greets the OSF UI runtime itself on every
// document (`osfui.hello`), so first open, F5, dev hot-reload and crash
// recovery are one path: ready -> full state replay -> events. There is no
// "on ready, re-request my data" convention to remember, and no member that
// requires one.
//
// Debugging is F12 Chromium DevTools. Every failure an author can cause is
// printed to this page's console with an `[osfui]` prefix — client-detected
// (rejections, timeouts, no-bridge) and runtime-detected alike (a send dropped for
// naming a request endpoint, an unknown endpoint, an endpoint handler that never
// answered). Set localStorage["osfui:trace"] = "1" and reload to log every
// envelope in both directions.
//
// This helper owns `osfui.onMessage` — with it loaded, do not assign onMessage
// yourself; use osfui.on() / osfui.state.on().

"use strict";

(function () {
  const g = (window.osfui = window.osfui || {});

  const events = new Map();      // event name -> Set<fn>
  const stateSubs = new Map();   // local key or "<mod>/<key>" -> Set<fn>
  const stateCache = new Map();  // local key or "<mod>/<key>" -> value
  const pending = new Map();     // request id -> { resolve, reject, timer, name, startedAt }
  let seq = 0;
  let ownMod = "";              // lower-case mod id from the first ready payload

  const bridged = typeof g.postMessage === "function";

  const TRACE = (function () {
    try { return window.localStorage.getItem("osfui:trace") === "1"; } catch (e) { return false; }
  })();

  const trace = TRACE
    ? function (direction, envelope, extra) {
        console.debug("[osfui] " + direction, envelope, extra === undefined ? "" : extra);
      }
    : function () {};

  // One sink for everything an author can get wrong, so DevTools shows it with
  // full object inspection instead of it living only in a log file the author
  // is not watching.
  function report(summary, detail) {
    if (detail === undefined) console.error("[osfui] " + summary);
    else console.error("[osfui] " + summary, detail);
  }

  function bridgeError(code, message) {
    const err = new Error(message);
    err.code = code;
    return err;
  }

  // ---------------------------------------------------------------------
  // send / request
  // ---------------------------------------------------------------------

  function post(envelope) {
    const json = JSON.stringify(envelope);
    trace("->", envelope);
    g.postMessage(json);
  }

  function sendPayload(values) {
    if (values.length === 0 || (values.length === 1 && values[0] === undefined)) return {};

    // Preserve the original generic/native API: one object is the payload.
    // Scalars and multiple values are Papyrus endpoint argument shorthand.
    if (values.length === 1 && values[0] !== null &&
        typeof values[0] === "object" && !Array.isArray(values[0])) {
      return values[0];
    }
    return { args: values };
  }

  function isRequestOptions(value) {
    if (value === null || typeof value !== "object" || Array.isArray(value)) return false;
    return Object.keys(value).length === 0 || Object.prototype.hasOwnProperty.call(value, "timeoutMs");
  }

  function requestCall(values) {
    let opts;

    // Preserve request(name, payload, opts), including the common
    // request(name, undefined, opts) spelling. A trailing timeout object also
    // works with the direct Papyrus shorthand.
    if (values.length > 1 &&
        (values[values.length - 1] === undefined || isRequestOptions(values[values.length - 1]))) {
      opts = values.pop();
    }

    return { payload: sendPayload(values), opts: opts };
  }

  g.send = function (name) {
    if (!bridged) return false;
    const values = Array.prototype.slice.call(arguments, 1);
    post({ kind: "send", name: String(name), payload: sendPayload(values) });
    return true;
  };

  g.request = function (name) {
    const endpoint = String(name);
    if (!bridged) {
      report('request "' + endpoint + '" failed: no-bridge (standalone preview)');
      return Promise.reject(bridgeError("no-bridge", "no bridge (standalone preview)"));
    }
    const call = requestCall(Array.prototype.slice.call(arguments, 1));
    const id = "q" + (++seq);
    const timeoutMs = call.opts && "timeoutMs" in call.opts ? call.opts.timeoutMs : 10000;
    return new Promise(function (resolve, reject) {
      let timer = 0;
      if (timeoutMs > 0) {
        timer = setTimeout(function () {
          pending.delete(id);
          const err = bridgeError("timeout", '"' + endpoint + '" got no reply within ' + timeoutMs + "ms");
          report('request "' + endpoint + '" failed: timeout after ' + timeoutMs + "ms");
          reject(err);
        }, timeoutMs);
      }
      pending.set(id, {
        resolve: resolve, reject: reject, timer: timer, name: endpoint,
        startedAt: TRACE ? Date.now() : 0,
      });
      post({ kind: "request", name: endpoint, id: id, payload: call.payload });
    });
  };

  // ---------------------------------------------------------------------
  // events
  // ---------------------------------------------------------------------

  g.on = function (event, fn) {
    if (typeof fn !== "function") throw new TypeError("osfui.on requires a handler");
    const name = String(event);
    let set = events.get(name);
    if (!set) events.set(name, (set = new Set()));
    set.add(fn);
    return function () { set.delete(fn); };
  };

  // ---------------------------------------------------------------------
  // state
  // ---------------------------------------------------------------------

  // Papyrus string interning means a key can arrive cased differently than the
  // script authored it, so state keys match case-insensitively throughout.
  function stateKey(key) { return String(key).toLowerCase(); }

  g.state = {
    get: function (key) { return stateCache.get(stateKey(key)); },
    on: function (key, fn) {
      if (typeof fn !== "function") throw new TypeError("osfui.state.on requires a handler");
      const wanted = stateKey(key);
      let set = stateSubs.get(wanted);
      if (!set) stateSubs.set(wanted, (set = new Set()));
      set.add(fn);
      // Replay synchronously: subscribing is a read, and a view that has to
      // ask "has it arrived yet?" is the bug this whole verb exists to remove.
      if (stateCache.has(wanted)) {
        try { fn(stateCache.get(wanted)); }
        catch (e) { report('state handler for "' + wanted + '" threw', e); }
      }
      return function () { set.delete(fn); };
    },
  };

  // ---------------------------------------------------------------------
  // inbound
  // ---------------------------------------------------------------------

  function publishState(address, value) {
    stateCache.set(address, value);
    const set = stateSubs.get(address);
    if (!set) return;
    for (const fn of Array.from(set)) {
      try { fn(value); }
      catch (e) { report('state handler for "' + address + '" threw', e); }
    }
  }

  function deliverState(mod, key, value) {
    const composite = stateKey(mod + "/" + key);
    publishState(composite, value);
    if (ownMod && stateKey(mod) === ownMod) publishState(stateKey(key), value);
  }

  function deliverEvent(name, payload) {
    // Runtime-detected protocol misuse arrives as an ordinary event so it lands in
    // this page's console with the same prefix as client-detected failures.
    if (name === "osfui.debug.error") {
      const p = payload || {};
      report("OSF UI runtime rejected " + (p.code || "a message") + ": " + (p.message || ""), p.detail || p);
      return;
    }
    const addresses = [name];
    const ownPrefix = ownMod + ".";
    if (ownMod && name.toLowerCase().startsWith(ownPrefix) && name.length > ownPrefix.length) {
      addresses.push(name.slice(ownPrefix.length));
    }
    for (const address of addresses) {
      const set = events.get(address);
      if (!set) continue;
      for (const fn of Array.from(set)) {
        try { fn(payload); }
        catch (e) { report('event handler for "' + address + '" threw', e); }
      }
    }
  }

  function settle(id, ok, payload) {
    const req = pending.get(id);
    if (!req) return;  // late or duplicate: a request settles exactly once
    pending.delete(id);
    if (req.timer) clearTimeout(req.timer);
    if (TRACE) trace("<-", { kind: ok ? "reply" : "error", id: id, payload: payload },
      (Date.now() - req.startedAt) + "ms");
    if (ok) {
      req.resolve(payload);
      return;
    }
    const p = payload || {};
    const err = bridgeError(p.code || "", p.message || p.code || "request failed");
    err.payload = p;
    report('request "' + req.name + '" failed: ' + (p.code || "(no code)") +
      (p.message ? " — " + p.message : ""), p);
    req.reject(err);
  }

  g.onMessage = function (json) {
    let message;
    try { message = JSON.parse(json); } catch (e) { return; }
    if (!message || typeof message.kind !== "string") return;
    if (message.kind !== "reply" && message.kind !== "error") trace("<-", message);

    switch (message.kind) {
      case "ready":
        if (!ownMod) {
          const info = message.payload || {};
          ownMod = info && typeof info.mod === "string" ? stateKey(info.mod) : "";
        }
        break;
      case "state":
        if (typeof message.mod === "string" && typeof message.key === "string") {
          deliverState(message.mod, message.key, message.value);
        }
        break;
      case "event":
        if (typeof message.name === "string") {
          deliverEvent(message.name,
            Object.prototype.hasOwnProperty.call(message, "payload") ? message.payload : {});
        }
        break;
      case "reply":
        settle(message.id, true,
          Object.prototype.hasOwnProperty.call(message, "payload") ? message.payload : {});
        break;
      case "error":
        settle(message.id, false, message.payload || {});
        break;
      default:
        break;
    }
  };

  // ---------------------------------------------------------------------
  // handshake — page-initiated, and the only boot path
  // ---------------------------------------------------------------------

  if (bridged) {
    g.send("osfui.hello");
  } else {
    // A plain-browser preview is not an authoring mistake.
    console.warn("[osfui] no bridge — standalone preview; state and requests are unavailable");
  }
})();
