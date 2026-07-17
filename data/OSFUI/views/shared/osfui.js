// osfui.js — the OSF UI bridge helper (bridge protocol 1.0, api-freeze item 5).
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
//                                   (payload.version = the running OSF UI)
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
//   osfui.applyAccent(el, hex)   -> apply a mod's `accent` hex to a subtree
//                                   (derives the kit's linked --osf-accent-*
//                                   set; invalid/missing hex clears it).
//   osfui.t(address, english, vars) -> active-locale override or authored
//                                   English, with {name} interpolation.
//   osfui.localize(root)         -> apply data-i18n* attributes under root.
//   osfui.i18nReady              -> first i18n.data catalog has arrived.
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

  // Per-mod theming (api-freeze item 9): a schema/manifest `accent` is ONE
  // hex; the kit reads a linked set of four tokens (accent, hover, strong,
  // quiet), so derive and set them together on the element — or clear the
  // whole set on a missing/invalid hex, so nothing leaks from a previously
  // themed subtree. This is the only theming mechanism (the old theme-class
  // enum is gone from osfui.css).
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
    if (!message || typeof message.type !== "string") return;
    if (message.type === "runtime.ready") {
      resolveReady(message.payload || {});
		// The catalog request is unconditional: every bridge-bearing host
		// serves i18n.get, and the i18n.data reply resolves i18nReady. A
		// failure still resolves (with the authored English) so views
		// awaiting i18nReady never hang; a host without the command (the dev
		// harness mock) refuses it fast and quietly.
		g.request("i18n.get").catch((e) => {
			if (!e || e.code !== "unknown-command") console.error("OSF UI localization load failed:", e);
			resolveI18n({ locale, strings });
		});
    }
		if (message.type === "i18n.data") {
			const payload = message.payload || {};
			locale = typeof payload.locale === "string" ? payload.locale : "en";
			strings = payload.strings && typeof payload.strings === "object" ? payload.strings : Object.create(null);
			document.documentElement.lang = locale;
			g.localize(document);
			resolveI18n(payload);
		}
    // Correlated reply: settle the request() promise...
    const rid = typeof message.requestId === "string" ? message.requestId : "";
    const req = rid && pending.get(rid);
    if (req) {
      pending.delete(rid);
      if (req.timer) clearTimeout(req.timer);
      const p = message.payload || {};
      if (message.type === "ui.error" || (message.type === "ui.result" && p.ok === false)) {
        const err = new Error(p.message || p.code || "request failed");
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
