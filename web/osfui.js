// OSF UI 2.0 helper, bridge protocol 2. API reference: sdk/osfui.d.ts.
// Load /shared/osfui.js before the view's script.
// Owns window.osfui.onMessage; use osfui.on() for events.
// Each document connects in this order: ready, state replay, events.
// Set localStorage["osfui:trace"] = "1" and reload to log messages.

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

  function report(summary, detail) {
    if (detail === undefined) console.error("[osfui] " + summary);
    else console.error("[osfui] " + summary, detail);
  }

  function bridgeError(code, message) {
    const err = new Error(message);
    err.code = code;
    return err;
  }

  function post(envelope) {
    const json = JSON.stringify(envelope);
    trace("->", envelope);
    g.postMessage(json);
  }

  function sendPayload(values) {
    if (values.length === 0 || (values.length === 1 && values[0] === undefined)) return {};

    // One plain object is the payload; scalars or multiple values are Papyrus args shorthand.
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

    // Trailing opts (or undefined) is split off, also after Papyrus shorthand args.
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
      try {
        post({ kind: "request", name: endpoint, id: id, payload: call.payload });
      } catch (error) {
        pending.delete(id);
        if (timer) clearTimeout(timer);
        reject(error);
      }
    });
  };

  g.on = function (event, fn) {
    if (typeof fn !== "function") throw new TypeError("osfui.on requires a handler");
    const name = String(event);
    let set = events.get(name);
    if (!set) events.set(name, (set = new Set()));
    set.add(fn);
    return function () { set.delete(fn); };
  };

  // Case-insensitive: Papyrus string interning can change a key's casing.
  function stateKey(key) { return String(key).toLowerCase(); }

  g.state = {
    get: function (key) { return stateCache.get(stateKey(key)); },
    on: function (key, fn) {
      if (typeof fn !== "function") throw new TypeError("osfui.state.on requires a handler");
      const wanted = stateKey(key);
      let set = stateSubs.get(wanted);
      if (!set) stateSubs.set(wanted, (set = new Set()));
      set.add(fn);
      if (stateCache.has(wanted)) {
        try { fn(stateCache.get(wanted)); }
        catch (e) { report('state handler for "' + wanted + '" threw', e); }
      }
      return function () { set.delete(fn); };
    },
  };

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

  if (bridged) {
    g.send("osfui.hello");
  } else {
    console.warn("[osfui] no bridge — standalone preview; state and requests are unavailable");
  }
})();
