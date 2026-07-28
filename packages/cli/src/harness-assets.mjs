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
  min-width: 0; min-height: 0; display: grid; grid-template-rows: auto minmax(120px, 1fr) auto 180px;
  border-left: 1px solid #31434d; background: #0d171d;
}
.panel h2 { margin: 0; padding: 10px 12px; font-size: 12px; letter-spacing: .12em; text-transform: uppercase; }
.traffic { min-height: 0; overflow: auto; margin: 0; padding: 8px 12px; list-style: none; border-block: 1px solid #263943; }
.traffic li { padding: 5px 0; border-bottom: 1px solid #1b2b33; overflow-wrap: anywhere; }
.traffic .out { color: #efc46b; }
.traffic .in { color: #78d0ad; }
.traffic .warn { color: #ff8c78; }
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
      <span id="view-id" class="view-id"></span>
      <span class="spacer"></span>
      <label>Width <input id="width" type="number" min="1" max="16384"></label>
      <label>Height <input id="height" type="number" min="1" max="16384"></label>
      <button id="apply-size" type="button">Apply</button>
      <label>Locale <input id="locale" value="en" size="8"></label>
      <button id="send-locale" type="button">Set</button>
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
