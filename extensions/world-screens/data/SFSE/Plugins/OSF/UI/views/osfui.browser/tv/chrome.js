"use strict";
(() => {
  const $ = id => document.getElementById(id);
  const prefix = "world-screens.browser.";
  const read = key => { try { return localStorage.getItem(prefix + key) || ""; } catch { return ""; } };
  const save = (key, value) => { try { localStorage.setItem(prefix + key, value); } catch {} };
  let state = { enabled: false, landing: true, fullscreen: false };
  let clearArmed = false, noticeTimer;
  const engines = { duckduckgo: "DuckDuckGo", google: "Google", bing: "Bing" };
  function selectEngine(value) {
    $("engine").value = value;
    $("engine").textContent = engines[value];
    $("engine").setAttribute("aria-label", "Search engine: " + engines[value] + ". Click to change.");
  }
  selectEngine(Object.hasOwn(engines, read("engine")) ? read("engine") : "duckduckgo");
  function notice(text) {
    clearTimeout(noticeTimer);
    $("notice").textContent = text;
    noticeTimer = setTimeout(() => { $("notice").textContent = ""; }, 7000);
  }
  async function request(action, payload = {}) {
    try { return await osfui.request("browser." + action, payload); }
    catch (error) { notice(error.message || "The browser could not complete that action."); return null; }
  }
  function apply(next) {
    state = next;
    document.body.classList.toggle("landing", !!state.landing);
    document.body.classList.toggle("interactive", !!state.fullscreen);
    for (const id of ["address", "go", "home", "engine", "set-home", "mute", "clear"])
      $(id).disabled = !state.enabled;
    $("back").disabled = !state.enabled || !state.canGoBack;
    $("forward").disabled = !state.enabled || !state.canGoForward;
    $("reload").disabled = !state.enabled || !state.url || state.url === "about:blank";
    $("reload").textContent = state.loading ? "×" : "↻";
    $("reload").title = state.loading ? "Stop loading" : "Reload";
    $("reload").setAttribute("aria-label", $("reload").title);
    $("mute").textContent = state.muted ? "Unmute" : state.audibleKnown && state.audible ? "Mute ♪" : "Mute";
    $("mute").setAttribute("aria-pressed", String(!!state.muted));
    const local = !state.url || state.url === "about:blank";
    if (document.activeElement !== $("address")) $("address").value = local ? "" : state.url;
    $("security").textContent = local ? "LOCAL" : state.secure ? "HTTPS" : "!";
    $("security").dataset.secure = String(!!state.secure);
    // The native source URL, never the page title, supplies the origin indicator.
    let origin = "Local landing page";
    try { if (!local) origin = new URL(state.url).origin; } catch {}
    $("security").title = origin + (local ? "" : state.secure ? " — HTTPS connection" : " — Connection not verified");
    document.title = state.title ? state.title + " — World Screens" : "World Screens browser";
    $("heading").textContent = !state.enabled ? "Web browsing is disabled" :
      state.error ? "This page couldn't load" : "Your screen. Your channel.";
    $("description").textContent = !state.enabled ?
      "Web browsing is disabled. The browsing option in Mods → OSF UI is coming in a later update." :
      state.error || "Search, open a website, or start a video. Close the controls and keep watching in the world.";
    $("hint").textContent = state.fullscreen ? "Ctrl+L or F6 selects the address bar. Esc closes the controls." : "Activate this screen to browse.";
    if (state.enabled && !state.loading && !state.error) save("last", local ? "" : state.url);
  }
  $("navigation").addEventListener("submit", event => {
    event.preventDefault();
    void request("navigate", { input: $("address").value, searchEngine: $("engine").value });
    $("address").blur();
  });
  $("address").addEventListener("blur", () => { $("address").value = state.url === "about:blank" ? "" : state.url || ""; });
  osfui.on("browser.focusAddress", () => { $("address").focus(); $("address").select(); });
  osfui.on("browser.state", apply);
  for (const action of ["back", "forward", "close"]) $(action).onclick = () => void request(action);
  $("reload").onclick = () => void request(state.loading ? "stop" : "reload");
  $("home").onclick = () => void request("home", { input: read("home"), searchEngine: $("engine").value });
  $("set-home").onclick = () => {
    save("home", state.url === "about:blank" ? "" : state.url || "");
    notice("Home page saved.");
  };
  $("engine").onclick = () => {
    const choices = Object.keys(engines);
    selectEngine(choices[(choices.indexOf($("engine").value) + 1) % choices.length]);
    save("engine", $("engine").value);
  };
  $("mute").onclick = () => void request("mute", { muted: !state.muted });
  $("clear").onclick = async () => {
    if (!clearArmed) {
      clearArmed = true; $("clear").textContent = "Confirm clear";
      notice("Clear cookies, sign-ins and browser preferences? Click Confirm clear.");
      setTimeout(() => { clearArmed = false; $("clear").textContent = "Clear data"; }, 7000);
      return;
    }
    clearArmed = false; $("clear").disabled = true; $("clear").textContent = "Clearing…";
    const result = await request("clearData");
    if (result) {
      for (const key of ["home", "last", "engine"]) save(key, "");
      selectEngine("duckduckgo");
      await request("home");
      notice(result.restartRequired ? "Data will be cleared when this screen's browser next starts." : "Browser data cleared.");
    }
    $("clear").textContent = "Clear data"; $("clear").disabled = !state.enabled;
  };
  // Core still owns pad Back. Other pad events stay in chrome; v1 uses a mouse.
  osfui.send("osfui.gamepadMode", { mode: "raw" });
  const last = read("last"); // Preserve before the initial blank-state replay.
  apply(state);
  request("getState").then(initial => {
    if (!initial) return;
    apply(initial);
    if (initial.enabled && (!initial.url || initial.url === "about:blank") && last)
      void request("navigate", { input: last, searchEngine: $("engine").value });
  });
})();
