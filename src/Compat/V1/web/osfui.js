// OSF UI 1.6 helper, adapted to the v2 transport. Source: v1.6.0 shared kit.
// Load before the view's own script: <script src="../../shared/osfui.js"></script>
// Decorates the injected `window.osfui` (a stub without a bridge). Frozen contract:
//
//   osfui.available()            -> bridge present (false = standalone preview)
//   osfui.ready                  -> Promise of the runtime.ready payload (payload.version)
//   osfui.send(command, fields)  -> fire-and-forget ui.command; false without a bridge
//   osfui.emit(command, fields)  -> alias for send()
//   osfui.action(name, ...args)  -> view ui.action (Papyrus RegisterForViewActionsArgs);
//                                   false without a bridge
//   osfui.request(command, fields, { timeoutMs }) -> Promise of { type, payload, requestId }.
//                                   Rejects (Error with .code) on ui.error, ui.result { ok:false },
//                                   timeout (default 10000 ms; 0 disables), or no bridge.
//   osfui.call(command, fields, opts) -> request(), resolving with the reply payload
//   osfui.viewReady()            -> first-paint readiness for readySignal:true; false without a bridge
//   osfui.on(type, fn)           -> fn(payload, message); returns unsubscribe
//   osfui.data.on(key, fn)       -> fn(value, payload, message) for cached Papyrus state;
//                                   replays the latest value immediately
//   osfui.data.get(key)          -> latest cached value, or undefined
//   osfui.papyrus.request(name, ...args) -> owning mod's Papyrus listener; resolves to value
//   osfui.applyAccent(el, hex)   -> derive --osf-accent-* on a subtree; invalid/missing hex clears
//   osfui.t(address, english, vars) -> locale override or English, with {name} interpolation
//   osfui.localize(root)         -> apply data-i18n* attributes under root
//   osfui.i18nReady              -> first i18n.data catalog arrived
//
// Owns `osfui.onMessage`; use osfui.on(). Request replies also reach on() subscribers.
// Commands without a typed reply resolve with `ui.result { ok, command, code?, message? }`
// ("delivered, not succeeded"); a typed reply suppresses that ack.

"use strict";

(function () {
  const g = (window.osfui = window.osfui || {});
  const listeners = new Map();  // type -> Set<fn>
  const pending = new Map();    // requestId -> { resolve, reject, timer, command, pluginRequest }
  const dataState = new Map();  // lower-case key -> { value, payload, message }
  let seq = 0;

	let strings = Object.create(null);
	let locale = "en";
	let resolveI18n;
	g.i18nReady = new Promise((r) => { resolveI18n = r; });
	g.locale = () => locale;
	g.t = (address, english, vars) => {
		let value = Object.prototype.hasOwnProperty.call(strings, address) ? strings[address] : english;
		value = value == null ? "" : String(value);
		return value.replace(/\{([A-Za-z0-9_]+)\}/g, (all, name) =>
			vars && Object.prototype.hasOwnProperty.call(vars, name) ? String(vars[name]) : all);
	};

	const sourceText = new WeakMap();
	const sourceAttrs = new WeakMap();
	g.localize = (root) => {
		root = root || document;
		const nodes = [];
		if (root.nodeType === 1 && root.matches("[data-i18n], [data-i18n-placeholder], [data-i18n-aria-label], [data-i18n-title]")) nodes.push(root);
		if (root.querySelectorAll) nodes.push(...root.querySelectorAll("[data-i18n], [data-i18n-placeholder], [data-i18n-aria-label], [data-i18n-title]"));
		for (const node of nodes) {
			if (node.dataset.i18n) {
				if (!sourceText.has(node)) sourceText.set(node, node.textContent);
				node.textContent = g.t(node.dataset.i18n, sourceText.get(node));
			}
			let attrs = sourceAttrs.get(node);
			if (!attrs) sourceAttrs.set(node, (attrs = Object.create(null)));
			for (const [dataName, attrName] of [["i18nPlaceholder", "placeholder"], ["i18nAriaLabel", "aria-label"], ["i18nTitle", "title"]]) {
				const address = node.dataset[dataName];
				if (!address) continue;
				if (!(attrName in attrs)) attrs[attrName] = node.getAttribute(attrName) || "";
				node.setAttribute(attrName, g.t(address, attrs[attrName]));
			}
		}
	};

  let resolveReady;
  g.ready = new Promise((r) => { resolveReady = r; });
  g.available = () => typeof g.postMessage === "function";

  g.send = (command, fields) => {
    if (!g.available()) return false;
    g.postMessage(JSON.stringify({ kind: "send", name: command, payload: Object.assign({ command }, fields || {}) }));
    return true;
  };
  g.emit = (command, fields) => g.send(command, fields);

  // Non-string args are coerced natively; a bare action omits `args`.
  g.action = (name, ...args) =>
    g.send("ui.action", args.length ? { action: name, args } : { action: name });

  g.viewReady = () => g.send("view.ready");

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
      pending.set(requestId, { resolve, reject, timer, command,
        pluginRequest: /^[a-z0-9-]+\.[a-z0-9-]+\..+/.test(command) });
      const payload = Object.assign({ command }, fields || {});
      // Private v2 transport metadata; the frozen Papyrus callback remains unchanged.
      if (command === "ui.papyrusRequest" && timeoutMs > 0)
        payload.__osfuiV1TimeoutMs = Math.min(60000, Math.max(1, Math.trunc(timeoutMs)));
      g.postMessage(JSON.stringify({ kind: "request", name: command, id: requestId, payload }));
    });
  };
  g.call = (command, fields, opts) =>
    g.request(command, fields, opts).then((message) => message.payload);
  g.papyrus = {
    action: (name, ...args) => g.action(name, ...args),
    request: (name, ...args) =>
      g.call("ui.papyrusRequest", { request: name, args }, { timeoutMs: 15000 })
        .then((payload) => payload.value),
  };

  g.on = (type, fn) => {
    let set = listeners.get(type);
    if (!set) listeners.set(type, (set = new Set()));
    set.add(fn);
    return () => set.delete(fn);
  };

  const dataKey = (key) => String(key).toLowerCase();
  const stateValue = (payload) => {
    if (payload && Object.prototype.hasOwnProperty.call(payload, "value")) return payload.value;
    if (payload && Object.prototype.hasOwnProperty.call(payload, "forms")) return payload.forms;
    return payload ? payload.values : undefined;
  };
  g.data = {
    get(key) {
      const current = dataState.get(dataKey(key));
      return current ? current.value : undefined;
    },
    on(key, fn) {
      if (typeof fn !== "function") throw new TypeError("osfui.data.on requires a handler");
      const wanted = dataKey(key);
      const deliver = (payload, message) => {
        if (!payload || dataKey(payload.key) !== wanted) return;
        fn(stateValue(payload), payload, message);
      };
      const offState = g.on("data.state", deliver);
      const offLegacy = g.on("data.push", deliver);
      const current = dataState.get(wanted);
      if (current) fn(current.value, current.payload, current.message);
      return () => {
        offState();
        offLegacy();
      };
    },
  };

  // Update all four accent colors together to clear the previous theme.
  const ACCENT_TOKENS = ["--osf-accent", "--osf-accent-hover", "--osf-accent-quiet", "--osf-accent-strong"];
  g.applyAccent = (el, hex) => {
    if (typeof hex === "string" && /^#[0-9a-fA-F]{6}([0-9a-fA-F]{2})?$/.test(hex)) {
      const rgb = [1, 3, 5].map((i) => parseInt(hex.slice(i, i + 2), 16));
      const mix = (target, t) => "#" + rgb.map((c) =>
        Math.round(c + (target - c) * t).toString(16).padStart(2, "0")).join("");
      el.style.setProperty("--osf-accent", hex.slice(0, 7));
      el.style.setProperty("--osf-accent-hover", mix(255, 0.34));
      el.style.setProperty("--osf-accent-strong", mix(0, 0.42));
      el.style.setProperty("--osf-accent-quiet", `rgba(${rgb[0]}, ${rgb[1]}, ${rgb[2]}, 0.14)`);
    } else {
      ACCENT_TOKENS.forEach((t) => el.style.removeProperty(t));
    }
  };

  g.onMessage = (json) => {
    let message;
    try { message = JSON.parse(json); } catch { return; }
    if (message && message.kind) {
      const incoming = message;
      switch (incoming.kind) {
        case "ready": message = { type: "runtime.ready", payload: incoming.payload }; break;
        case "event": message = { type: incoming.name, payload: incoming.payload }; break;
        case "state": message = { type: "data.state", payload: { mod: incoming.mod, key: incoming.key, value: incoming.value } }; break;
        case "reply":
          message = incoming.payload && incoming.payload.__osfuiV1Reply === true
            ? { type: incoming.payload.type, payload: incoming.payload.payload, requestId: incoming.id }
            : { type: "ui.result", payload: incoming.payload, requestId: incoming.id };
          break;
        case "error": message = { type: "ui.error", payload: incoming.payload, requestId: incoming.id }; break;
        default: return;
      }
    }
    if (!message || typeof message.type !== "string") return;
    if (message.type === "data.reset") {
      // Hidden documents can outlive a save; clear cached state before the new session's arrives.
      const keys = message.payload && Array.isArray(message.payload.keys)
        ? message.payload.keys.map(dataKey) : [...dataState.keys()];
      const old = keys.map(key => dataState.get(key)).filter(Boolean);
      keys.forEach(key => dataState.delete(key));
      for (const current of old) {
        const payload = { ...current.payload, value: null };
        for (const fn of [...(listeners.get("data.state") || [])]) {
          try { fn(payload, { type: "data.state", payload }); } catch (e) { console.error("osfui.on handler failed:", e); }
        }
      }
    }
    if (message.type === "runtime.ready") {
      resolveReady(message.payload || {});
        resolveI18n({ locale, strings });
    }
		if (message.type === "i18n.data") {
			const payload = message.payload || {};
			locale = typeof payload.locale === "string" ? payload.locale : "en";
			strings = payload.strings && typeof payload.strings === "object" ? payload.strings : Object.create(null);
			document.documentElement.lang = locale;
			g.localize(document);
			resolveI18n(payload);
		}
    if (message.type === "data.state" || message.type === "data.push") {
      const payload = message.payload || {};
      if (typeof payload.key === "string" && payload.key) {
        dataState.set(dataKey(payload.key), {
          value: stateValue(payload),
          payload,
          message,
        });
      }
    }
    const rid = typeof message.requestId === "string" ? message.requestId : "";
    const req = rid && pending.get(rid);
    if (req) {
      const p = message.payload || {};
      // Accept an auto-ack for this command when the handler sent no reply.
      // Ignore acks for other commands.
      const foreignAck = req.pluginRequest && message.type === "ui.result" &&
        p.ok === true && p.command !== req.command;
      if (!foreignAck) {
        pending.delete(rid);
        if (req.timer) clearTimeout(req.timer);
        if (message.type === "ui.error" || (message.type === "ui.result" && p.ok === false)) {
          const err = new Error(p.message || p.code || "request failed");
          err.code = p.code || "";
          err.reply = message;
          req.reject(err);
        } else {
          req.resolve(message);
        }
      }
    }
    const set = listeners.get(message.type);
    if (set) {
      for (const fn of [...set]) {
        try { fn(message.payload || {}, message); } catch (e) { console.error("osfui.on handler failed:", e); }
      }
    }
  };
  // v2 readiness is page-initiated.
  if (g.available()) g.send("osfui.hello");
})();
