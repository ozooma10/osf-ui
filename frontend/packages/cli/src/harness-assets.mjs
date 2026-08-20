export const HARNESS_CSS = String.raw`
:root { color-scheme: dark; font: 13px/1.4 "Segoe UI", system-ui, sans-serif; color: #e8f2f6; background: #070b0e; }
* { box-sizing: border-box; }
html, body, .app { width: 100%; height: 100%; margin: 0; overflow: hidden; }
.app { display: grid; grid-template-rows: auto minmax(0, 1fr); }
.toolbar { display: flex; align-items: center; gap: 12px; min-height: 42px; padding: 6px 10px; background: #101b22; border-bottom: 1px solid #31434d; }
.brand { font-weight: 700; letter-spacing: .08em; }
.view-id { color: #8fc8dc; }
.spacer { flex: 1; }
#tools { display: contents; }
button, select { min-height: 28px; padding: 3px 8px; font: inherit; color: inherit; background: #111d24; border: 1px solid #37505c; }
button { cursor: pointer; }
.status { color: #9fb1b9; }
.error { color: #ff8c78; }
.stage-shell { min-width: 0; min-height: 0; overflow: hidden; display: grid; place-items: center; background: #10161a; }
.stage { flex: none; transform-origin: center; box-shadow: 0 0 0 1px #55707c, 0 10px 40px #000a; }
.stage iframe { display: block; width: 100%; height: 100%; border: 0; background: transparent; }
`;

export const HARNESS_HTML = String.raw`<!doctype html>
<html lang="en">
<head>
  <meta charset="utf-8">
  <meta name="viewport" content="width=device-width, initial-scale=1">
  <title>OSF UI Animation Harness</title>
  <link rel="stylesheet" href="/__osfui/harness.css">
</head>
<body>
  <main class="app">
    <header class="toolbar">
      <span class="brand">OSF UI HARNESS</span>
      <span id="view-id" class="view-id"></span>
      <span class="spacer"></span>
      <span id="tools"></span>
      <button id="reload" type="button">Reload</button>
      <span id="status" class="status">Starting…</span>
    </header>
    <section class="stage-shell"><div id="stage" class="stage"><iframe id="view" title="View preview"></iframe></div></section>
  </main>
  <script type="module" src="/__osfui/harness.js"></script>
</body>
</html>`;
