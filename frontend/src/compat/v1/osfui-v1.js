
"use strict";

(function () {
  const params = new URLSearchParams(window.location.search);
  if (params.get("osfui-api") !== "1") return;

  const core = window.osfui;
  if (!core || typeof core.send !== "function" || typeof core.request !== "function") return;
  // The injected transport keeps its native inbound sink on the original
  // bridge object. Capture the strict handler before replacing window.osfui,
  // then install the compatibility wrapper back into that transport slot.
  const coreOnMessage = typeof core.onMessage === "function" ? core.onMessage.bind(core) : function () {};

  const listeners = new Map();
  const dataState = new Map();
  const platformState = new Map();
  const captureWaiters = [];
  let requestSeq = 0;

  function dataKey(key) { return String(key).toLowerCase(); }

  function stateValue(payload) {
    if (payload && Object.prototype.hasOwnProperty.call(payload, "value")) return payload.value;
    if (payload && Object.prototype.hasOwnProperty.call(payload, "forms")) return payload.forms;
    return payload ? payload.values : undefined;
  }

  function dispatch(type, payload, message) {
    const set = listeners.get(type);
    if (!set) return;
    const delivered = payload === undefined ? {} : payload;
    for (const fn of Array.from(set)) {
      try { fn(delivered, message); }
      catch (error) { console.error("osfui.on handler failed:", error); }
    }
  }

  function rememberData(payload, message) {
    if (!payload || typeof payload.key !== "string" || !payload.key) return;
    dataState.set(dataKey(payload.key), {
      value: stateValue(payload),
      payload: payload,
      message: message,
    });
  }

  const stateReplies = {
    "osfui/settings": "settings.data",
    "osfui/views": "views.data",
    "osfui/i18n": "i18n.data",
    "osfui/diagnostics": "diagnostics.data",
  };

  function translateInbound(message) {
    if (!message || typeof message.kind !== "string") return;
    if (message.kind === "ready") {
      dispatch("runtime.ready", message.payload || {}, {
        type: "runtime.ready",
        payload: message.payload || {},
      });
      return;
    }
    if (message.kind === "event" && typeof message.name === "string") {
      const legacy = {
        type: message.name,
        payload: Object.prototype.hasOwnProperty.call(message, "payload") ? message.payload : {},
      };
      if (message.name === "data.push") rememberData(legacy.payload, legacy);
      if (message.name === "settings.captured") {
        const index = captureWaiters.findIndex(function (waiter) {
          return (!waiter.mod || dataKey(waiter.mod) === dataKey(legacy.payload.mod)) &&
            (!waiter.key || dataKey(waiter.key) === dataKey(legacy.payload.key));
        });
        if (index !== -1) {
          const waiter = captureWaiters.splice(index, 1)[0];
          waiter.resolve(legacy.payload);
          return;
        }
      }
      dispatch(message.name, legacy.payload, legacy);
      return;
    }
    if (message.kind !== "state" || typeof message.mod !== "string" ||
        typeof message.key !== "string") return;

    const scoped = dataKey(message.mod + "/" + message.key);
    platformState.set(scoped, message.value);
    const registryType = stateReplies[scoped];
    if (registryType) {
      const registry = { type: registryType, payload: message.value || {} };
      dispatch(registryType, registry.payload, registry);
      return;
    }

    const payload = { mod: message.mod, key: message.key, value: message.value };
    const legacy = { type: "data.state", payload: payload };
    rememberData(payload, legacy);
    dispatch("data.state", payload, legacy);
  }

  function translateEndpoint(command, fields) {
    const name = String(command);
    const payload = fields || {};
    if (name === "ui.action") {
      const args = Array.isArray(payload.args) ? payload.args :
        (Object.prototype.hasOwnProperty.call(payload, "arg") ? [payload.arg] : []);
      return {
        name: String(payload.action || ""),
        payload: { args: args },
      };
    }
    if (name === "ui.papyrusRequest") {
      return {
        name: String(payload.request || ""),
        payload: { args: Array.isArray(payload.args) ? payload.args : [] },
      };
    }
    if (name === "hud.show") return { name: "menu.open", payload: payload };
    if (name === "hud.hide") return { name: "menu.close", payload: payload };
    return { name: name, payload: payload };
  }

  function replyType(command) {
    if (command === "settings.get") return "settings.data";
    if (command === "settings.set") return "settings.ack";
    if (command === "settings.captureKey") return "settings.captured";
    if (command === "settings.reset") return "settings.data";
    if (command === "views.get") return "views.data";
    if (command === "i18n.get") return "i18n.data";
    if (command === "diagnostics.get") return "diagnostics.data";
    if (command === "ui.papyrusRequest") return "papyrus.result";
    if (command === "ping") return "runtime.pong";
    return "ui.result";
  }

  const registryReads = {
    "settings.get": "osfui/settings",
    "views.get": "osfui/views",
    "i18n.get": "osfui/i18n",
    "diagnostics.get": "osfui/diagnostics",
  };

  function readState(scoped) {
    const cached = core.state.get(scoped);
    if (cached !== undefined) return Promise.resolve(cached);
    return new Promise(function (resolve) {
      let off = function () {};
      off = core.state.on(scoped, function (value) {
        off();
        resolve(value);
      });
    });
  }

  function requestPayload(command, translated, fields, opts) {
    const state = registryReads[command];
    if (state) return readState(state);
    if (command === "settings.reset") {
      return new Promise(function (resolve, reject) {
        let subscribing = true;
        let refreshed;
        let hasRefresh = false;
        let requestDone = false;
        let off = function () {};
        let fallback = 0;
        function finish() {
          if (!requestDone || !hasRefresh) return;
          if (fallback) clearTimeout(fallback);
          off();
          resolve(refreshed || {});
        }
        off = core.state.on("osfui/settings", function (value) {
          if (subscribing) return;
          refreshed = value;
          hasRefresh = true;
          finish();
        });
        subscribing = false;
        core.request(translated.name, translated.payload, opts).then(function () {
          requestDone = true;
          finish();
          if (!hasRefresh) {
            fallback = setTimeout(function () {
              off();
              resolve(core.state.get("osfui/settings") || {});
            }, 1000);
          }
        }, function (error) {
          off();
          reject(error);
        });
      });
    }
    if (command === "settings.captureKey") {
      return new Promise(function (resolve, reject) {
        const waiter = {
          mod: String((fields && fields.mod) || ""),
          key: String((fields && fields.key) || ""),
          resolve: resolve,
        };
        captureWaiters.push(waiter);
        const timeoutMs = opts && "timeoutMs" in opts ? opts.timeoutMs : 10000;
        let timer = 0;
        if (timeoutMs > 0) {
          timer = setTimeout(function () {
            const index = captureWaiters.indexOf(waiter);
            if (index !== -1) captureWaiters.splice(index, 1);
            const error = new Error('"settings.captureKey" got no reply within ' + timeoutMs + "ms");
            error.code = "timeout";
            reject(error);
          }, timeoutMs);
        }
        const originalResolve = waiter.resolve;
        waiter.resolve = function (value) {
          if (timer) clearTimeout(timer);
          originalResolve(value);
        };
        core.request(translated.name, translated.payload, opts).catch(function (error) {
          const index = captureWaiters.indexOf(waiter);
          if (index !== -1) captureWaiters.splice(index, 1);
          if (timer) clearTimeout(timer);
          reject(error);
        });
      });
    }
    return core.request(translated.name, translated.payload, opts).then(function (payload) {
      // The current request contract resolves a Papyrus reply as the raw scalar.
      // Rebuild the released 1.x `{ value }` document at the compatibility edge.
      if (command === "ui.papyrusRequest") return { value: payload };
      if (command === "settings.set" && payload && payload.ok === undefined) {
        return Object.assign({ ok: true }, payload);
      }
      return payload;
    }, function (error) {
      if (command !== "settings.set") throw error;
      const detail = error && error.payload ? error.payload : {};
      return {
        mod: fields && fields.mod,
        key: fields && fields.key,
        ok: false,
        code: detail.code || (error && error.code) || "",
        message: detail.message || (error && error.message) || "settings write failed",
      };
    });
  }

  function legacyRequest(command, fields, opts) {
    const original = String(command);
    const translated = translateEndpoint(original, fields);
    const requestId = "q" + (++requestSeq);
    return requestPayload(original, translated, fields || {}, opts).then(function (payload) {
      const wrapped = payload && payload.__osfuiV1Reply;
      const message = {
        type: wrapped && typeof wrapped.type === "string" ? wrapped.type : replyType(original),
        requestId: requestId,
        payload: wrapped && Object.prototype.hasOwnProperty.call(wrapped, "payload") ?
          wrapped.payload : (payload || {}),
      };
      dispatch(message.type, message.payload, message);
      return message;
    }, function (error) {
      const payload = error && error.payload ? error.payload : {
        code: (error && error.code) || "",
        message: (error && error.message) || "request failed",
      };
      const message = { type: "ui.error", requestId: requestId, payload: payload };
      dispatch("ui.error", payload, message);
      if (error && typeof error === "object") error.reply = message;
      throw error;
    });
  }

  const requestOnly = new Set([
    "menu.open", "menu.close", "hud.show", "hud.hide", "setViewHidden",
    "ping", "settings.get", "views.get", "i18n.get", "diagnostics.get",
    "settings.set", "settings.reset", "settings.captureKey",
    "osfui.setViewAutoStart",
    "ui.papyrusRequest",
  ]);

  const legacy = {
    postMessage: typeof core.postMessage === "function" ? core.postMessage.bind(core) : core.postMessage,
    onMessage: function (json) {
      let message;
      try { message = JSON.parse(json); } catch (error) { message = null; }
      translateInbound(message);
      coreOnMessage(json);
    },
    available: function () { return Boolean(core.available); },
    ready: core.ready,
    send: function (command, fields) {
      const original = String(command);
      if (original === "osfui.textFocus") return true;
      const translated = translateEndpoint(original, fields);
      if (requestOnly.has(original)) {
        legacyRequest(original, fields).catch(function () {});
        return Boolean(core.available);
      }
      return core.send(translated.name, translated.payload);
    },
    request: legacyRequest,
    call: function (command, fields, opts) {
      return legacyRequest(command, fields, opts).then(function (message) { return message.payload; });
    },
    on: function (type, fn) {
      if (typeof fn !== "function") throw new TypeError("osfui.on requires a handler");
      const name = String(type);
      let set = listeners.get(name);
      if (!set) listeners.set(name, (set = new Set()));
      set.add(fn);
      return function () { set.delete(fn); };
    },
  };

  legacy.emit = function (command, fields) { return legacy.send(command, fields); };
  legacy.action = function (name) {
    const args = Array.prototype.slice.call(arguments, 1);
    return legacy.send("ui.action", args.length ? { action: name, args: args } : { action: name });
  };
  legacy.data = {
    get: function (key) {
      const current = dataState.get(dataKey(key));
      return current ? current.value : undefined;
    },
    on: function (key, fn) {
      if (typeof fn !== "function") throw new TypeError("osfui.data.on requires a handler");
      const wanted = dataKey(key);
      const deliver = function (payload, message) {
        if (!payload || dataKey(payload.key) !== wanted) return;
        fn(stateValue(payload), payload, message);
      };
      const offState = legacy.on("data.state", deliver);
      const offPush = legacy.on("data.push", deliver);
      const current = dataState.get(wanted);
      if (current) fn(current.value, current.payload, current.message);
      return function () { offState(); offPush(); };
    },
  };
  legacy.papyrus = {
    call: function () { return core.papyrus.call.apply(core.papyrus, arguments); },
    action: function () { return legacy.action.apply(legacy, arguments); },
    request: function (name) {
      const args = Array.prototype.slice.call(arguments, 1);
      return legacy.call("ui.papyrusRequest", { request: name, args: args }, { timeoutMs: 15000 })
        .then(function (payload) { return payload ? payload.value : undefined; });
    },
  };
  legacy.i18nReady = core.i18n.ready;
  legacy.locale = function () { return core.i18n.locale; };
  legacy.t = function () { return core.i18n.t.apply(core.i18n, arguments); };
  legacy.localize = function () { return core.i18n.localize.apply(core.i18n, arguments); };
  legacy.applyAccent = function () { return core.theme.applyAccent.apply(core.theme, arguments); };

  core.onMessage = legacy.onMessage;
  window.osfui = legacy;
})();
