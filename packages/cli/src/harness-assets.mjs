// Inline page assets for the standalone authoring harness. The browser
// JavaScript (shell, bootstrap) lives as real files in src/browser/ — served
// verbatim by harness-plugin.mjs — so it gets syntax checking and tests;
// only the small, rarely-churned HTML/CSS stay as template literals here.
export const HARNESS_CSS = String.raw`
:root {
  color-scheme: dark;
  font: 13px/1.4 "Segoe UI", system-ui, sans-serif;
  color: #e8f2f6;
  background: #091015;
}
* { box-sizing: border-box; }
html, body { width: 100%; height: 100%; margin: 0; overflow: hidden; }
button, input, select, textarea {
  font: inherit;
  color: inherit;
  background: #111d24;
  border: 1px solid #37505c;
}
button { min-height: 28px; padding: 3px 10px; cursor: pointer; }
button:hover { border-color: #8fc8dc; background: #19303b; }
input, select { height: 28px; padding: 3px 6px; }
input[type="number"] { width: 76px; }
.app { display: grid; grid-template-rows: auto minmax(0, 1fr); width: 100%; height: 100%; }
.toolbar {
  display: flex; align-items: center; gap: 8px; min-height: 42px; padding: 6px 10px;
  background: #101b22; border-bottom: 1px solid #31434d; white-space: nowrap;
}
.brand { font-weight: 700; letter-spacing: .08em; margin-right: 6px; }
/* Mock-registered dev controls render inline with the fixed toolbar items. */
#tools { display: contents; }
.toolbar .on, .traffic-bar .on { border-color: #8fc8dc; background: #19303b; }
.view-id { color: #8fc8dc; overflow: hidden; text-overflow: ellipsis; }
.spacer { flex: 1; }
.status { color: #9fb1b9; }
.workspace { display: grid; grid-template-columns: minmax(0, 1fr) 360px; min-height: 0; }
.stage-shell {
  min-width: 0; min-height: 0; overflow: auto; display: grid; place-items: center;
  padding: 24px; background: #070b0e;
}
.stage-shell.checker {
  background-color: #10161a;
  background-image:
    linear-gradient(45deg, #182229 25%, transparent 25%),
    linear-gradient(-45deg, #182229 25%, transparent 25%),
    linear-gradient(45deg, transparent 75%, #182229 75%),
    linear-gradient(-45deg, transparent 75%, #182229 75%);
  background-size: 24px 24px;
  background-position: 0 0, 0 12px, 12px -12px, -12px 0;
}
.stage {
  position: relative; flex: none; overflow: hidden; background: transparent;
  box-shadow: 0 0 0 1px #55707c, 0 10px 40px #000a;
  transform-origin: center;
}
.stage iframe { display: block; width: 100%; height: 100%; border: 0; background: transparent; }
.panel {
  min-width: 0; min-height: 0; display: grid;
  grid-template-rows: auto auto minmax(120px, 1fr) auto 180px;
  border-left: 1px solid #31434d; background: #0d171d;
}
.panel h2 { margin: 0; padding: 10px 12px 6px; font-size: 12px; letter-spacing: .12em; text-transform: uppercase; }
.traffic-bar { display: flex; gap: 6px; padding: 0 12px 8px; }
.traffic-bar input { flex: 1; min-width: 0; }
.traffic { min-height: 0; overflow: auto; margin: 0; padding: 0; list-style: none; border-block: 1px solid #263943; }
/* One exchange: a clickable headline row plus the raw envelope it expands to. */
.row { border-bottom: 1px solid #16242b; }
.row-head {
  display: flex; align-items: baseline; gap: 7px; width: 100%; min-height: 0;
  padding: 5px 12px; border: 0; background: none; text-align: left;
}
.row-head:hover { background: #14232b; border-color: transparent; }
.row-head:disabled { cursor: default; opacity: 1; }
.row .time { color: #63808c; font-variant-numeric: tabular-nums; flex: none; }
.row .dir { flex: none; font-size: 10px; }
.row .title { flex: none; font-weight: 600; }
/* The distinguishing detail: it yields the width, the expanded body has it all. */
.row .detail { flex: 1; min-width: 0; color: #9fb1b9; overflow: hidden; text-overflow: ellipsis; white-space: nowrap; }
.row .tag, .row .count {
  flex: none; margin-left: auto; padding: 0 5px; border-radius: 9px;
  background: #1b2f39; color: #9fb1b9; font-size: 11px;
}
.row .count + .tag, .row .tag ~ .count { margin-left: 0; }
.row-body {
  margin: 0; padding: 2px 12px 10px 30px; overflow-x: auto;
  color: #9fb1b9; font-family: Consolas, monospace; font-size: 12px;
}
.row.open { background: #101e26; }
.row.out .dir, .row.out .title { color: #efc46b; }
.row.in .dir, .row.in .title { color: #78d0ad; }
.row.warn .title, .row.warn .detail { color: #ff8c78; }
/* Harness notes are prose, not envelopes: let them wrap and take the row. */
.row.note .title { flex: 1; min-width: 0; color: #9fb1b9; font-weight: 400; overflow-wrap: anywhere; }
.event-editor { display: grid; grid-template-rows: auto 1fr auto; min-height: 0; padding: 8px 12px; gap: 6px; }
.event-editor label { color: #9fb1b9; }
.event-editor textarea { width: 100%; min-height: 80px; resize: none; padding: 7px; font-family: Consolas, monospace; }
.error { color: #ff8c78; }
@media (max-width: 980px) {
  .workspace { grid-template-columns: minmax(0, 1fr); }
  .panel { display: none; }
}
`;

export const HARNESS_HTML = String.raw`<!doctype html>
<html lang="en">
<head>
  <meta charset="utf-8">
  <meta name="viewport" content="width=device-width, initial-scale=1">
  <title>OSF UI View Harness</title>
  <link rel="stylesheet" href="/__osfui/harness.css">
</head>
<body>
  <main class="app">
    <header class="toolbar">
      <span class="brand">OSF UI HARNESS</span>
      <select id="view-select" title="Switch the previewed view (no server restart)"></select>
      <span id="view-id" class="view-id"></span>
      <span class="spacer"></span>
      <label>Width <input id="width" type="number" min="1" max="16384"
        title="The view's declared width. Informational: the staged width follows the pane, as the game widens the page to the output aspect."></label>
      <label>Height <input id="height" type="number" min="1" max="16384"
        title="The view's declared reference row height — this is what pins the stage scale."></label>
      <button id="apply-size" type="button">Apply</button>
      <button id="stage-mode" type="button">Stage</button>
      <span id="tools"></span>
      <label>Locale <input id="locale" value="en" size="8" list="locale-list"
        title="Push an i18n.data locale switch. 'pseudo' pseudo-localizes every localized string ([åççéñŧš] + padding) so hardcoded text and tight layouts stand out."></label>
      <datalist id="locale-list">
        <option value="en"></option><option value="pseudo"></option><option value="de"></option>
        <option value="fr"></option><option value="es"></option><option value="ja"></option>
      </datalist>
      <button id="send-locale" type="button">Set</button>
      <input id="hotkey-key" size="9" placeholder="hotkey key" title="Setting key for the ui.hotkey injector (mod is the project's modId)">
      <button id="inject-hotkey" type="button" title="Inject a ui.hotkey message">Hotkey</button>
      <button id="inject-lb" type="button" title="Inject a ui.gamepad LB press (down edge + release)">LB</button>
      <button id="inject-rb" type="button" title="Inject a ui.gamepad RB press (down edge + release)">RB</button>
      <button id="visibility" type="button">Hide</button>
      <button id="checker" type="button">Checker</button>
      <button id="reload" type="button">Reload</button>
      <span id="status" class="status">Starting…</span>
    </header>
    <section class="workspace">
      <div id="stage-shell" class="stage-shell checker">
        <div id="stage" class="stage"><iframe id="view" title="View preview"></iframe></div>
      </div>
      <aside class="panel">
        <h2>Bridge traffic</h2>
        <div class="traffic-bar">
          <input id="traffic-filter" type="search" placeholder="Filter rows"
            title="Show only rows whose headline or JSON contains this text">
          <button id="traffic-pause" type="button"
            title="Hold new rows so you can read; held messages are added on resume">Pause</button>
          <button id="traffic-clear" type="button" title="Empty the log">Clear</button>
        </div>
        <ol id="traffic" class="traffic"></ol>
        <h2>Send native event</h2>
        <div class="event-editor">
          <label for="event-json">Envelope JSON</label>
          <textarea id="event-json" spellcheck="false">{
  "type": "data.state",
  "payload": {
    "key": "example",
    "value": "Hello from the harness"
  }
}</textarea>
          <button id="send-event" type="button">Send to view</button>
        </div>
      </aside>
    </section>
  </main>
  <script type="module" src="/__osfui/harness.js"></script>
</body>
</html>`;
