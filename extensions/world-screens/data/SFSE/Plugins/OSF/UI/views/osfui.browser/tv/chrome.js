"use strict";
(() => {
  const $ = id => document.getElementById(id);
  const body = document.body;
  const prefix = "world-screens.browser.";
  const read = key => { try { return localStorage.getItem(prefix + key) || ""; } catch { return ""; } };
  const save = (key, value) => {
    try { if (value) localStorage.setItem(prefix + key, value); else localStorage.removeItem(prefix + key); } catch {}
  };
  const readList = key => {
    try {
      const list = JSON.parse(read(key) || "[]");
      return Array.isArray(list) ? list.filter(i => i && typeof i.url === "string" && /^https?:\/\//i.test(i.url))
        .map(i => ({ url: i.url, title: typeof i.title === "string" ? i.title.slice(0, 300) : "" })) : [];
    } catch { return []; }
  };
  const engines = { duckduckgo: "DuckDuckGo", google: "Google", bing: "Bing" };
  const tints = ["#b39bcf", "#8db8cd", "#a1bd7d", "#ce99b2", "#d4ab72", "#a3a8dc"]; // OSF Settings badge palette
  const icon = {
    chevron: '<svg class="chevron" viewBox="0 0 10 16" aria-hidden="true"><path d="m2 2 6 6-6 6"/></svg>',
    left: '<svg viewBox="0 0 10 16" aria-hidden="true"><path d="M8 2 2 8l6 6"/></svg>',
    right: '<svg viewBox="0 0 10 16" aria-hidden="true"><path d="m2 2 6 6-6 6"/></svg>',
    play: '<span class="play" aria-hidden="true"><svg class="fill" viewBox="0 0 14 16"><path d="M2 1.5v13L13 8z"/></svg></span>',
    search: '<svg viewBox="0 0 20 20" aria-hidden="true"><circle cx="9" cy="9" r="6"/><path d="m13.5 13.5 5 5"/></svg>'
  };

  let state = { enabled: false, landing: true, fullscreen: false };
  let engine = Object.hasOwn(engines, read("engine")) ? read("engine") : "duckduckgo";
  let pins = readList("pins"), recent = readList("recent");
  let section = "channels", selectedKey = "", confirmClear = false, clearing = false, editing = false;
  let noticeText = "", noticeTimer, appliedKey = "", renderedKey = "", models = new Map();

  const blank = url => !url || url === "about:blank";
  const page = () => !blank(state.url);
  const host = url => { try { return new URL(url).host.replace(/^www\./i, ""); } catch { return ""; } };
  const label = item => item.title || host(item.url) || item.url;
  const screens = () => Math.max(0, Number(state.screens) || 0);
  const screensText = n => n + (n === 1 ? " bound screen" : " bound screens");
  const pinned = url => pins.some(p => p.url === url);
  function monogram(url) {
    const parts = host(url).split(".").filter(Boolean);
    const base = parts.length > 1 ? parts[parts.length - 2] : parts[0] || "";
    return base.replace(/[^a-z0-9]/gi, "").slice(0, 2).toUpperCase() || "?";
  }
  function tint(url) {
    let hash = 0;
    for (const c of host(url)) hash = (hash * 31 + c.charCodeAt(0)) >>> 0;
    return tints[hash % tints.length];
  }
  function el(tag, className, text) {
    const node = document.createElement(tag);
    if (className) node.className = className;
    if (text !== undefined) node.textContent = text;
    return node;
  }

  function notice(text) {
    clearTimeout(noticeTimer);
    noticeText = text;
    $("notice").textContent = text;
    renderFooterStatus();
    noticeTimer = setTimeout(() => { noticeText = ""; $("notice").textContent = ""; renderFooterStatus(); }, 7000);
  }
  async function request(action, payload = {}) {
    try { return await osfui.request("browser." + action, payload); }
    catch (error) { notice(error.message || "The browser could not complete that action."); return null; }
  }
  const navigate = input => request("navigate", { input, searchEngine: engine });
  function openGuide(target) {
    section = target; selectedKey = ""; confirmClear = false;
    void request("guide", { open: true });
  }
  const closeGuide = () => void request("guide", { open: false });

  function setEngine(next) { engine = next; save("engine", next); updateHint(); render(); }
  function cycleEngine(step) {
    const keys = Object.keys(engines);
    setEngine(keys[(keys.indexOf(engine) + step + keys.length) % keys.length]);
  }
  function remember(url, title) {
    if (blank(url)) return;
    if (recent[0] && recent[0].url === url) {
      if (!title || recent[0].title === title) return;
      recent[0].title = title;
    } else recent = [{ url, title: title || "" }, ...recent.filter(r => r.url !== url)].slice(0, 12);
    save("recent", JSON.stringify(recent));
  }
  function togglePin(url, title) {
    if (pinned(url)) { pins = pins.filter(p => p.url !== url); notice("Removed from Channels."); }
    else { pins = [...pins, { url, title: title || "" }].slice(-24); notice("Pinned to Channels."); }
    save("pins", JSON.stringify(pins));
    render();
  }

  // ── Toolbar ────────────────────────────────────────────────────
  function apply(next) {
    state = next || state;
    // Audio diagnostics include changing poses/counters, but no chrome controls
    // render them. Preserve the latest state without rewriting identical DOM or
    // browser history on every diagnostic update.
    const presentation = { ...state };
    delete presentation.audio;
    const key = JSON.stringify(presentation);
    if (key === appliedKey) return;
    appliedKey = key;
    const hasPage = page();
    body.classList.toggle("landing", !!state.landing);
    body.classList.toggle("interactive", !!state.fullscreen);
    body.classList.toggle("disabled", !state.enabled);
    body.classList.toggle("loading", !!state.loading && !state.landing);
    body.classList.toggle("insecure", hasPage && !state.secure);
    if (state.landing && editing) stopEditing();
    $("back").disabled = !state.enabled || !state.canGoBack;
    $("forward").disabled = !state.enabled || !state.canGoForward;
    $("reload").disabled = !state.enabled || !hasPage;
    $("reload").title = state.loading ? "Stop loading" : "Reload";
    $("reload").setAttribute("aria-label", $("reload").title);
    for (const id of ["open-guide", "open-settings", "mute", "readout"]) $(id).disabled = !state.enabled;
    // The origin comes from the native source URL; the page title is page-controlled.
    $("origin").textContent = hasPage ? host(state.url) || state.url : "";
    $("page-title").textContent = state.title || (hasPage ? state.url : "");
    $("readout").setAttribute("aria-label", "Address " + (hasPage ? state.url : "") +
      (hasPage && !state.secure ? ", connection not verified" : "") + ". Edit address.");
    if (!editing) $("address").value = hasPage ? state.url : "";
    const count = screens();
    $("screens").hidden = count < 1;
    $("screens-label").textContent = "On " + screensText(count);
    $("mute").setAttribute("aria-pressed", String(!!state.muted));
    $("mute-label").textContent = state.muted ? "Muted" : state.audibleKnown !== false && state.audible ? "Playing" : "Sound on";
    $("mute").title = state.muted ? "Unmute every screen on this channel" : "Mute every screen on this channel";
    document.title = state.title ? state.title + " — World Screens" : "World Screens browser";
    if (state.enabled && !state.loading && !state.error) {
      save("last", hasPage ? state.url : "");
      if (hasPage) remember(state.url, state.title);
    }
    render();
  }
  function looksLikeAddress(value) {
    return /^https?:\/\//i.test(value) || (!/\s/.test(value) && /\.[a-z]{2,}(?:[:/?#]|$)/i.test(value));
  }
  function updateHint() {
    const value = $("address").value.trim();
    $("address-hint").textContent = !value ? "" : looksLikeAddress(value) ? "Enter · Open" : "Enter · Search " + engines[engine];
  }
  function startEditing() {
    if (!state.enabled || state.landing) return;
    editing = true; body.classList.add("editing");
    $("address").value = page() ? state.url : "";
    updateHint(); $("address").focus(); $("address").select();
  }
  function stopEditing() {
    editing = false; body.classList.remove("editing");
    $("address").value = page() ? state.url : "";
  }
  $("readout").onclick = startEditing;
  $("address").addEventListener("input", updateHint);
  $("address").addEventListener("blur", stopEditing);
  $("navigation").addEventListener("submit", event => {
    event.preventDefault();
    const value = $("address").value;
    $("address").blur();
    if (value.trim()) void navigate(value);
  });
  $("back").onclick = () => void request("back");
  $("forward").onclick = () => void request("forward");
  $("reload").onclick = () => void request(state.loading ? "stop" : "reload");
  $("open-guide").onclick = () => openGuide("channels");
  $("open-settings").onclick = () => openGuide("settings");
  $("mute").onclick = () => void request("mute", { muted: !state.muted });
  $("close").onclick = () => void request("close");
  osfui.on("browser.focusAddress", () => {
    const search = $("guide-address");
    if (state.landing && search) { search.focus(); search.select(); } else startEditing();
  });
  osfui.on("browser.state", apply);

  // ── Guide ──────────────────────────────────────────────────────
  // Rows are { key, title, sub, value, badge, kind, run, remove, pin, cycle, detail, prompts }.
  function channelRows() {
    const rows = [], count = screens();
    if (state.error && page()) {
      rows.push({ key: "error", kind: "warning", title: "This page couldn't load",
        sub: host(state.url) + " · " + state.error, run: () => void request("reload"),
        detail: { title: "This page couldn't load", origin: state.url, body: state.error,
          actions: [{ label: "Try again", primary: true, run: () => void request("reload") }] },
        prompts: [["Enter", "Try again"]] });
    } else if (page() && state.guide) {
      rows.push({ divider: "Now playing" }, { key: "now", kind: "tall", play: true, title: state.title || host(state.url),
        sub: host(state.url) + (count ? " · on " + screensText(count) : ""), run: closeGuide,
        detail: { badge: state.url, title: state.title || host(state.url), origin: state.url,
          body: "Return to the page. Every screen on this channel keeps playing it.",
          actions: [{ label: "Back to page", primary: true, run: closeGuide }] },
        prompts: [["Enter", "Back to page"]] });
    } else if (!page() && recent[0]) {
      const last = recent[0].url, title = label(recent[0]);
      rows.push({ divider: "Resume" }, { key: "resume", kind: "tall", play: true, title,
        sub: host(last) + " · last played on this channel", run: () => void navigate(last),
        detail: { badge: last, title, origin: last,
          body: "Pick up where this channel left off. Playback starts from the beginning.",
          actions: [{ label: "Resume", primary: true, run: () => void navigate(last) }] },
        prompts: [["Enter", "Resume"]] });
    }
    rows.push({ divider: "Pinned" });
    for (const pin of pins) rows.push(pageRow("pin", pin, true));
    if (!pins.length) rows.push({ key: "pins-empty", kind: "empty", title: "Pin pages from Channel settings while they play" });
    return rows;
  }
  function pageRow(kind, item, isPin) {
    const watch = () => void navigate(item.url);
    const toggle = () => togglePin(item.url, item.title);
    const isPinned = pinned(item.url);
    return {
      key: kind + ":" + item.url, title: label(item), value: host(item.url), badge: item.url, run: watch,
      remove: isPin ? toggle : undefined, pin: isPin ? undefined : toggle,
      detail: { badge: item.url, title: label(item), origin: item.url,
        body: "Opens on every screen tuned to this channel. Sign-ins and cookies stay in the OSF UI browser profile.",
        actions: [{ label: "Watch", primary: true, run: watch }, { label: isPinned ? "Unpin" : "Pin", run: toggle }] },
      prompts: [["Enter", "Watch"], isPin ? ["Del", "Unpin"] : ["P", isPinned ? "Unpin" : "Pin"]]
    };
  }
  function recentRows() {
    const rows = [{ divider: "Recently played" }];
    for (const item of recent) rows.push(pageRow("recent", item, false));
    if (!recent.length) rows.push({ key: "recent-empty", kind: "empty", title: "Pages you play here show up in this list" });
    return rows;
  }
  function settingsRows() {
    const rows = [], hasPage = page() && !state.error;
    if (hasPage) {
      const isPinned = pinned(state.url);
      rows.push({ divider: "This page" }, { key: "pin-page", title: "Pin to Channels", value: isPinned ? "Pinned" : "Not pinned",
        run: () => togglePin(state.url, state.title),
        detail: { title: "Pin to Channels", body: "Pinned pages appear in Channels, ready to play on any screen tuned to this channel." },
        prompts: [["Enter", isPinned ? "Unpin" : "Pin"]] });
    }
    rows.push({ divider: "Playback" }, { key: "engine", title: "Search with", cycle: step => cycleEngine(step),
      value: engines[engine], run: () => cycleEngine(1),
      detail: { title: "Search with", body: "Used when what you type is not a web address." },
      prompts: [["←→", "Change"]] });
    rows.push({ key: "sound", title: "Sound", cycle: () => void request("mute", { muted: !state.muted }),
      value: state.muted ? "Muted" : "On", run: () => void request("mute", { muted: !state.muted }),
      detail: { title: "Sound", body: "Mute silences every screen on this channel. The page keeps playing." },
      prompts: [["Enter", state.muted ? "Unmute" : "Mute"]] });
    if (page()) rows.push({ key: "off", title: "Turn off channel", value: "Stops every screen",
      run: () => void request("home", { input: "" }),
      detail: { title: "Turn off channel", body: "Stops the page and returns every screen on this channel to this guide." },
      prompts: [["Enter", "Turn off"]] });
    rows.push({ divider: "Privacy" }, { key: "forget", title: "Forget recent pages", value: recent.length ? String(recent.length) : "None",
      run: () => { recent = []; save("recent", ""); notice("Recent pages forgotten."); render(); },
      detail: { title: "Forget recent pages", body: "Clears the Recent list. Pins, cookies and sign-ins stay." },
      prompts: [["Enter", "Forget"]] });
    rows.push(confirmClear ? { key: "clear", kind: "danger", confirm: true, title: "Sign out of every site?",
      sub: "Removes cookies, sign-ins, pins and recent pages.",
      detail: { title: "Clear cookies and sign-ins", body: "Websites keep their data in the OSF UI browser profile. This removes all of it." },
      prompts: [["Enter", "Clear data"]], run: clearData } : { key: "clear", title: clearing ? "Clearing…" : "Clear cookies and sign-ins",
      chevron: !clearing, run: clearing ? undefined : () => { confirmClear = true; selectedKey = "clear"; render(true); },
      detail: { title: "Clear cookies and sign-ins", body: "Signs you out of every site and resets pins, recent pages and the search engine." },
      prompts: [["Enter", "Clear"]] });
    return rows;
  }
  async function clearData() {
    confirmClear = false; clearing = true; render(true);
    const result = await request("clearData");
    clearing = false;
    if (result) {
      for (const key of ["last", "engine", "pins", "recent", "home"]) save(key, "");
      engine = "duckduckgo"; pins = []; recent = [];
      await request("home", { input: "" });
      notice(result.restartRequired ? "Data will be cleared when this screen's browser next starts." : "Browser data cleared.");
    }
    render(true);
  }

  function makeRow(model) {
    if (model.divider) return el("div", "divider", model.divider);
    const row = el(model.confirm ? "div" : "button", "row");
    if (model.kind) row.classList.add(model.kind);
    row.dataset.key = model.key;
    if (model.kind === "empty") { row.disabled = true; row.append(el("span", "text", model.title)); return row; }
    if (!model.confirm) { row.type = "button"; if (!model.run) row.disabled = true; else row.onclick = model.run; }
    if (model.play) row.insertAdjacentHTML("beforeend", icon.play);
    if (model.badge) {
      const badge = el("span", "badge", monogram(model.badge));
      badge.style.setProperty("--tint", tint(model.badge));
      badge.setAttribute("aria-hidden", "true");
      row.append(badge);
    }
    const text = el("span", "text");
    text.append(el("span", "", model.title));
    if (model.sub) text.append(el("span", "sub", model.sub));
    row.append(text);
    if (model.cycle) {
      const cycle = el("span", "cycle");
      cycle.insertAdjacentHTML("beforeend", icon.left);
      cycle.append(el("b", "", model.value));
      cycle.insertAdjacentHTML("beforeend", icon.right);
      row.append(cycle);
    } else if (model.value) row.append(el("span", "value", model.value));
    if (model.confirm) {
      const actions = el("span", "inline-actions");
      const cancel = el("button", "", "Cancel"), confirm = el("button", "primary", "Clear data");
      cancel.type = confirm.type = "button";
      cancel.onclick = () => { confirmClear = false; render(true); };
      confirm.onclick = clearData;
      actions.append(cancel, confirm);
      row.append(actions);
    } else if ((model.chevron || model.badge || model.play || model.kind === "warning") && model.run) row.insertAdjacentHTML("beforeend", icon.chevron);
    const pick = () => select(model.key);
    row.addEventListener("mouseenter", pick);
    row.addEventListener("focusin", pick);
    return row;
  }
  function select(key) {
    if (!models.has(key)) return;
    selectedKey = key;
    for (const row of $("list").querySelectorAll(".row")) row.classList.toggle("selected", row.dataset.key === key);
    renderDetail(models.get(key)); renderPrompts(models.get(key));
  }
  function renderDetail(model) {
    const detail = $("detail");
    detail.replaceChildren();
    const info = model && model.detail;
    if (info) {
      const head = el("div", "head");
      if (info.badge) {
        const badge = el("span", "badge large", monogram(info.badge));
        badge.style.setProperty("--tint", tint(info.badge));
        badge.setAttribute("aria-hidden", "true");
        head.append(badge);
      }
      const titles = el("div");
      titles.append(el("h2", "", info.title));
      if (info.origin) titles.append(el("div", "origin", info.origin));
      head.append(titles);
      detail.append(head);
      if (info.body) detail.append(el("p", "", info.body));
      if (info.actions && state.fullscreen) {
        const actions = el("div", "actions");
        for (const action of info.actions) {
          const button = el("button", action.primary ? "primary" : "", action.label);
          button.type = "button"; button.onclick = action.run;
          actions.append(button);
        }
        detail.append(actions);
      }
    }
    const orbit = el("div", "orbit");
    orbit.setAttribute("aria-hidden", "true");
    orbit.innerHTML = "<i></i><i></i><i></i><i></i><i class=\"planet a\"></i><i class=\"planet b\"></i><i class=\"planet c\"></i>";
    detail.append(orbit);
  }
  function promptButton(key, text, run) {
    const button = el("button", "prompt");
    button.type = "button";
    button.append(el("kbd", "", key), text);
    if (run) button.onclick = run; else button.tabIndex = -1;
    return button;
  }
  function renderPrompts(model) {
    const prompts = $("prompts");
    prompts.replaceChildren();
    if (!state.fullscreen) return;
    for (const [key, text] of (model && model.prompts) || []) {
      const run = key === "Enter" ? model.run : key === "Del" ? model.remove : key === "P" ? model.pin : undefined;
      prompts.append(promptButton(key, text, run));
    }
    prompts.append(state.guide ? promptButton("Esc", "Back to page", closeGuide) :
      promptButton("Esc", "Return to world", () => void request("close")));
  }
  function renderFooterStatus() {
    const status = $("footer-status");
    status.classList.toggle("notice", !!noticeText);
    status.textContent = noticeText || (!state.fullscreen ? "Activate this screen to browse." :
      !state.enabled ? "" : "Close the browser and the screens keep playing.");
  }
  function renderDisabled(list) {
    $("section-title").textContent = "Web browsing is off";
    $("section-meta").textContent = "Restart required after changing";
    list.append(el("p", "lead", "This screen can play real websites, but browsing has to be turned on first."));
    const steps = el("ol", "steps");
    for (const [n, text] of [["01", "Open Mods, then OSF UI - World Screens, then Browsing"],
      ["02", "Turn on “Allow web browsing on World Screens”"], ["03", "Restart Starfield"]]) {
      const item = el("li");
      item.append(el("b", "", n), text);
      steps.append(item);
    }
    list.append(steps);
    const card = el("div", "card");
    card.style.marginTop = "28px";
    card.append(el("div", "heading", "What turning it on means"),
      el("div", "", "Sites load for real, with their own cookies and sign-ins, stored in the OSF UI browser profile — not in your saves."),
      el("div", "", "Only Browser TV screens go online. Other OSF UI views stay offline."));
    list.append(card);
    models = new Map();
    renderDetail(null);
    renderPrompts(null);
  }
  function render(force) {
    if (!state.landing) return;
    const key = JSON.stringify([section, state.enabled, state.guide, state.url, state.title, state.error, state.muted,
      state.fullscreen, state.screens, engine, pins, recent, confirmClear, clearing]);
    if (!force && key === renderedKey) return;
    renderedKey = key;
    const list = $("list");
    const search = $("guide-address");
    const kept = search && { value: search.value, focused: document.activeElement === search };
    const focusedKey = document.activeElement && document.activeElement.closest && document.activeElement.closest("#list .row")?.dataset.key;
    list.replaceChildren();

    for (const button of $("sections").children) button.setAttribute("aria-selected", String(button.dataset.section === section));
    $("count-channels").textContent = pins.length ? String(pins.length) : "";
    $("count-recent").textContent = recent.length ? String(recent.length) : "";
    $("status-browsing-dot").className = state.enabled ? "on" : "";
    $("status-browsing").textContent = state.enabled ? "Web browsing on" : "Web browsing off";
    $("status-screens-row").hidden = screens() < 1;
    $("status-screens").textContent = screensText(screens()) + " on this channel";
    renderFooterStatus();
    if (!state.enabled) { renderDisabled(list); return; }

    const titles = { channels: "Channels", recent: "Recent", settings: "Channel settings" };
    $("section-title").textContent = titles[section];
    $("section-meta").textContent = section === "settings" ? (page() ? host(state.url) : "") :
      section === "recent" ? "Pages played on this channel" : "Every screen on this channel shares playback";
    if (section === "channels") {
      const form = el("form", "row search");
      form.setAttribute("autocomplete", "off");
      form.insertAdjacentHTML("beforeend", icon.search);
      const input = el("input");
      input.id = "guide-address";
      input.placeholder = "Search " + engines[engine] + " or enter an address";
      input.setAttribute("aria-label", "Search or enter an address");
      input.spellcheck = false; input.maxLength = 8192;
      if (kept) input.value = kept.value;
      form.append(input, el("kbd", "", "F6"));
      form.addEventListener("submit", event => { event.preventDefault(); if (input.value.trim()) void navigate(input.value); });
      list.append(form);
      if (kept && kept.focused) input.focus();
    }
    const rows = section === "recent" ? recentRows() : section === "settings" ? settingsRows() : channelRows();
    models = new Map(rows.filter(r => r.key).map(r => [r.key, r]));
    for (const model of rows) list.append(makeRow(model));
    const first = rows.find(r => r.key && r.kind !== "empty");
    select(models.has(selectedKey) ? selectedKey : first ? first.key : "");
    if (!models.has(selectedKey)) { renderDetail(null); renderPrompts(null); }
    if (focusedKey) list.querySelector(`.row[data-key="${CSS.escape(focusedKey)}"]`)?.focus();
  }
  for (const button of $("sections").children) {
    button.onclick = () => { section = button.dataset.section; selectedKey = ""; confirmClear = false; render(true); };
  }
  // Keyboard: arrows move through rows, ←/→ change cycling values, Del unpins, P pins.
  document.addEventListener("keydown", event => {
    if (!state.landing || !state.fullscreen || event.ctrlKey || event.altKey || event.metaKey) return;
    const rows = [...$("list").querySelectorAll(".row:not(:disabled)")].filter(r => r.dataset.key || r.classList.contains("search"));
    const inSearch = event.target && event.target.id === "guide-address";
    const current = rows.findIndex(r => r.contains(document.activeElement));
    const model = models.get(selectedKey);
    if (event.key === "ArrowDown" || event.key === "ArrowUp") {
      const next = rows[Math.min(rows.length - 1, Math.max(0, current + (event.key === "ArrowDown" ? 1 : -1)))];
      if (next) { event.preventDefault(); (next.querySelector("input, .primary") || next).focus(); }
    } else if (inSearch) {
      return;
    } else if ((event.key === "ArrowLeft" || event.key === "ArrowRight") && model && model.cycle) {
      event.preventDefault(); model.cycle(event.key === "ArrowLeft" ? -1 : 1);
    } else if (event.key === "Delete" && model && model.remove) {
      event.preventDefault(); model.remove();
    } else if ((event.key === "p" || event.key === "P") && model && model.pin) {
      event.preventDefault(); model.pin();
    }
  });

  // Core still owns pad Back. Other pad events stay in chrome; v1 uses a mouse.
  osfui.send("osfui.gamepadMode", { mode: "raw" });
  const last = read("last"); // Preserve before the initial blank-state replay.
  apply(state);
  request("getState").then(initial => {
    if (!initial) return;
    apply(initial);
    if (initial.enabled && blank(initial.url) && last) void navigate(last);
  });
})();
