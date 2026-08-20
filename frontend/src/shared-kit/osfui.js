// osfui.js — OSF UI bridge helper (mod API 2.0, bridge protocol 2.0).
//
// Load it like the shared stylesheet, BEFORE your view's own script:
//   <script src="../../shared/osfui.js"></script>
//
// Decorates the native-injected `window.osfui` (creating a stub when no bridge
// is present, e.g. a plain browser). The public API has four verbs, chosen by
// semantics, plus three sugar namespaces that add ergonomics and never new
// semantics:
//
//   osfui.available            -> bridge present (false = standalone preview)
//   osfui.ready                -> Promise of OSF UI runtime handshake info; REJECTS with
//                                 code "no-bridge" in a plain browser rather
//                                 than hanging forever
//
//   osfui.send(name, payload?)            -> one-way. Returns "posted locally",
//                                            never a remote outcome. Wanting one
//                                            means it is a request.
//   osfui.request(name, payload?, opts?)  -> Promise of the reply PAYLOAD.
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
//   osfui.state.get(key)                  -> latest value, or undefined.
//
//   osfui.papyrus.call / .float            -> advanced GLOBAL-call escape hatch
//   osfui.i18n.ready / .locale / .t / .localize
//   osfui.theme.applyAccent(el, hex)
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
  let readySeen = false;
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
  // available / ready
  // ---------------------------------------------------------------------

  Object.defineProperty(g, "available", { get: function () { return bridged; }, enumerable: true });

  let resolveReady, rejectReady;
  g.ready = new Promise(function (resolve, reject) { resolveReady = resolve; rejectReady = reject; });
  // Attaching a handler here marks the promise handled, so a plain-browser
  // preview does not spew "Uncaught (in promise)" before the view's own await.
  g.ready.catch(function () {});

  // ---------------------------------------------------------------------
  // send / request
  // ---------------------------------------------------------------------

  function post(envelope) {
    const json = JSON.stringify(envelope);
    trace("->", envelope);
    g.postMessage(json);
  }

  g.send = function (name, payload) {
    if (!bridged) return false;
    post({ kind: "send", name: String(name), payload: payload || {} });
    return true;
  };

  g.request = function (name, payload, opts) {
    const endpoint = String(name);
    if (!bridged) {
      report('request "' + endpoint + '" failed: no-bridge (standalone preview)');
      return Promise.reject(bridgeError("no-bridge", "no bridge (standalone preview)"));
    }
    const id = "q" + (++seq);
    const timeoutMs = opts && "timeoutMs" in opts ? opts.timeoutMs : 10000;
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
      post({ kind: "request", name: endpoint, id: id, payload: payload || {} });
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
  // sugar: advanced GLOBAL Papyrus calls
  // ---------------------------------------------------------------------

  g.papyrus = {
    float: function (value) {
      return { $papyrus: "float", value: Number(value) };
    },
    call: function (script, fn) {
      const args = Array.prototype.slice.call(arguments, 2);
      return g.send("papyrus.call", { script: String(script), function: String(fn), args: args });
    },
  };

  // ---------------------------------------------------------------------
  // sugar: i18n — pure functions over the "osfui/i18n" state key
  // ---------------------------------------------------------------------

  let strings = Object.create(null);
  let locale = "en";
  let resolveI18n;
  const i18nReady = new Promise(function (r) { resolveI18n = r; });

  const sourceText = new WeakMap();
  const sourceAttrs = new WeakMap();
  const I18N_ATTRS = [["i18nPlaceholder", "placeholder"], ["i18nAriaLabel", "aria-label"], ["i18nTitle", "title"]];
  const I18N_SELECTOR = "[data-i18n], [data-i18n-placeholder], [data-i18n-aria-label], [data-i18n-title]";

  function translate(address, english, vars) {
    let value = Object.prototype.hasOwnProperty.call(strings, address) ? strings[address] : english;
    value = value == null ? "" : String(value);
    return value.replace(/\{([A-Za-z0-9_]+)\}/g, function (all, name) {
      return vars && Object.prototype.hasOwnProperty.call(vars, name) ? String(vars[name]) : all;
    });
  }

  function localize(root) {
    root = root || document;
    const nodes = [];
    if (root.nodeType === 1 && root.matches(I18N_SELECTOR)) nodes.push(root);
    if (root.querySelectorAll) nodes.push.apply(nodes, root.querySelectorAll(I18N_SELECTOR));
    for (const node of nodes) {
      if (node.dataset.i18n) {
        if (!sourceText.has(node)) sourceText.set(node, node.textContent);
        node.textContent = translate(node.dataset.i18n, sourceText.get(node));
      }
      let attrs = sourceAttrs.get(node);
      if (!attrs) sourceAttrs.set(node, (attrs = Object.create(null)));
      for (const pair of I18N_ATTRS) {
        const address = node.dataset[pair[0]];
        if (!address) continue;
        if (!(pair[1] in attrs)) attrs[pair[1]] = node.getAttribute(pair[1]) || "";
        node.setAttribute(pair[1], translate(address, attrs[pair[1]]));
      }
    }
  }

  g.i18n = { ready: i18nReady, t: translate, localize: localize };
  Object.defineProperty(g.i18n, "locale", { get: function () { return locale; }, enumerable: true });

  // ---------------------------------------------------------------------
  // sugar: theme — never touches the wire
  // ---------------------------------------------------------------------

  // A settings-schema `accent` is one hex, but the kit reads a linked set of
  // four tokens, so derive and set them together — or clear the whole set on a
  // missing/invalid hex, so nothing leaks from a previously themed subtree.
  const ACCENT_TOKENS = ["--osf-accent", "--osf-accent-hover", "--osf-accent-quiet", "--osf-accent-strong"];
  g.theme = {
    applyAccent: function (el, hex) {
      if (!el || !el.style) return;
      if (typeof hex === "string" && /^#[0-9a-fA-F]{6}([0-9a-fA-F]{2})?$/.test(hex)) {
        const rgb = [1, 3, 5].map(function (i) { return parseInt(hex.slice(i, i + 2), 16); });
        const mix = function (target, t) {
          return "#" + rgb.map(function (c) {
            return Math.round(c + (target - c) * t).toString(16).padStart(2, "0");
          }).join("");
        };
        el.style.setProperty("--osf-accent", hex.slice(0, 7));
        el.style.setProperty("--osf-accent-hover", mix(255, 0.34));
        el.style.setProperty("--osf-accent-strong", mix(0, 0.42));
        el.style.setProperty("--osf-accent-quiet", "rgba(" + rgb[0] + ", " + rgb[1] + ", " + rgb[2] + ", 0.14)");
      } else {
        ACCENT_TOKENS.forEach(function (t) { el.style.removeProperty(t); });
      }
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
    if (composite === "osfui/i18n") {
      const catalog = value || {};
      locale = typeof catalog.locale === "string" ? catalog.locale : "en";
      strings = catalog.strings && typeof catalog.strings === "object" ? catalog.strings : Object.create(null);
      document.documentElement.lang = locale;
      localize(document);
      resolveI18n({ locale: locale, strings: strings });
    }
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
        if (!readySeen) {
          readySeen = true;
          const info = message.payload || {};
          ownMod = info && typeof info.mod === "string" ? stateKey(info.mod) : "";
          resolveReady(info);
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
    rejectReady(bridgeError("no-bridge", "no bridge (standalone preview)"));
    // A plain-browser preview is not an authoring mistake, so this is a notice
    // rather than an error: the view still renders with its inline English.
    console.warn("[osfui] no bridge — standalone preview; state and requests are unavailable");
    resolveI18n({ locale: locale, strings: strings });
  }
})();
