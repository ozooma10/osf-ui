// Browser-side assets shared by the standalone authoring harness.
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

export const HARNESS_JS = String.raw`
const $ = (id) => document.getElementById(id);
const frame = $('view');
const stage = $('stage');
const shell = $('stage-shell');
const traffic = $('traffic');
let meta;
let visible = true;
let checker = true;

function log(direction, value, level = '') {
  const item = document.createElement('li');
  item.className = level || direction;
  const stamp = new Date().toLocaleTimeString();
  item.textContent = stamp + ' ' + (direction === 'out' ? 'WEB → NATIVE ' : 'NATIVE → WEB ') +
    (typeof value === 'string' ? value : JSON.stringify(value));
  traffic.append(item);
  while (traffic.children.length > 200) traffic.firstElementChild.remove();
  traffic.scrollTop = traffic.scrollHeight;
}

function send(message) {
  if (!frame.contentWindow) return;
  if (meta && !meta.nativeBridge) {
    log('in', 'Bridge disabled by manifest.permissions.nativeBridge', 'warn');
    return;
  }
  frame.contentWindow.postMessage({ source: 'osfui-harness', kind: 'deliver', message }, location.origin);
  log('in', message);
}

function setSize(width, height) {
  width = Math.max(1, Math.min(16384, Number(width) || 1600));
  height = Math.max(1, Math.min(16384, Number(height) || 900));
  $('width').value = String(width);
  $('height').value = String(height);
  stage.style.width = width + 'px';
  stage.style.height = height + 'px';
  requestAnimationFrame(scaleStage);
}

function scaleStage() {
  if (!meta) return;
  const width = Number($('width').value);
  const height = Number($('height').value);
  const availableWidth = Math.max(1, shell.clientWidth - 48);
  const availableHeight = Math.max(1, shell.clientHeight - 48);
  const scale = Math.min(1, availableWidth / width, availableHeight / height);
  stage.style.transform = 'scale(' + scale + ')';
  stage.style.margin = ((height * scale - height) / 2) + 'px ' +
    ((width * scale - width) / 2) + 'px';
  $('status').textContent = width + '×' + height + ' at ' + Math.round(scale * 100) + '%';
}

async function loadMeta(navigate = true) {
  meta = await fetch('/__osfui/meta.json', { cache: 'no-store' }).then((r) => r.json());
  $('view-id').textContent = meta.qualifiedId + ' — ' + (meta.title || meta.qualifiedId);
  setSize(meta.width, meta.height);
  if (navigate) frame.src = meta.viewUrl;
}

window.addEventListener('message', (event) => {
  if (event.origin !== location.origin || !event.data || event.data.source !== 'osfui-harness') return;
  if (event.data.kind === 'traffic') log(event.data.direction, event.data.message, event.data.level);
  if (event.data.kind === 'ready') {
    $('status').textContent = meta.nativeBridge ? 'Bridge ready' : 'Bridge disabled by manifest';
    if (meta.nativeBridge) {
      send({ type: 'ui.visibility', payload: { visible, reason: 'overlay' } });
    }
  }
});
window.addEventListener('resize', scaleStage);

$('apply-size').addEventListener('click', () => setSize($('width').value, $('height').value));
$('reload').addEventListener('click', () => frame.contentWindow?.location.reload());
$('checker').addEventListener('click', () => {
  checker = !checker;
  shell.classList.toggle('checker', checker);
  $('checker').textContent = checker ? 'Checker' : 'Black';
});
$('visibility').addEventListener('click', () => {
  visible = !visible;
  send({ type: 'ui.visibility', payload: { visible, reason: 'overlay' } });
  $('visibility').textContent = visible ? 'Hide' : 'Show';
});
$('send-locale').addEventListener('click', () => {
  const locale = $('locale').value.trim() || 'en';
  if (!meta.nativeBridge) {
    log('in', 'Bridge disabled by manifest.permissions.nativeBridge', 'warn');
    return;
  }
  frame.contentWindow?.postMessage(
    { source: 'osfui-harness', kind: 'control', action: 'locale', locale },
    location.origin,
  );
});
$('send-event').addEventListener('click', () => {
  try {
    const message = JSON.parse($('event-json').value);
    if (!message || typeof message.type !== 'string') throw new Error('message.type is required');
    send(message);
  } catch (error) {
    log('in', String(error), 'warn');
  }
});

await loadMeta();
`;

export const BOOTSTRAP_JS = String.raw`
(() => {
  'use strict';
  const SOURCE = 'osfui-harness';
  const queued = [];
  let fixture = { state: {}, requests: {}, locales: {} };

  const report = (direction, message, level = '') => {
    parent.postMessage({ source: SOURCE, kind: 'traffic', direction, message, level }, location.origin);
  };
  const deliver = (message) => {
    const text = JSON.stringify(message);
    const receive = window.osfui && window.osfui.onMessage;
    if (typeof receive === 'function') {
      receive(text);
    } else {
      queued.push(text);
    }
  };
  const flush = () => {
    const receive = window.osfui && window.osfui.onMessage;
    if (typeof receive !== 'function') return false;
    while (queued.length) receive(queued.shift());
    return true;
  };
  const reply = (requestId, type, payload) => {
    if (!requestId) return;
    const message = { type, requestId, payload };
    report('in', message);
    queueMicrotask(() => deliver(message));
  };
  const commandResponse = (command) => {
    const value = fixture.requests && fixture.requests[command];
    if (value === undefined) return null;
    if (value && typeof value === 'object' && typeof value.$type === 'string') {
      return { type: value.$type, payload: value.payload ?? {} };
    }
    return { type: 'mock.result', payload: value };
  };
  const postMessage = (text) => {
    let message;
    try { message = JSON.parse(text); } catch {
      report('out', text, 'warn');
      return;
    }
    report('out', message);
    if (!message || message.type !== 'ui.command') return;
    const payload = message.payload || {};
    const command = String(payload.command || '');
    if (command === 'i18n.get') {
      const locale = fixture.locale || 'en';
      const strings = fixture.locales?.[locale] || {};
      reply(message.requestId, 'i18n.data', { mod: payload.mod || __OSFUI_HARNESS_META__.modId, locale, strings });
      return;
    }
    if (command === 'ui.papyrusRequest') {
      const response = commandResponse('papyrus.' + String(payload.request || ''));
      if (response) reply(message.requestId, 'papyrus.result', { value: response.payload });
      else reply(message.requestId, 'ui.error', {
        code: 'mock-unhandled', command, message: 'No osfui.mock.json response for Papyrus request "' + payload.request + '".'
      });
      return;
    }
    const response = commandResponse(command);
    if (response) {
      reply(message.requestId, response.type, response.payload);
      return;
    }
    const builtIn = new Set([
      'close', 'menu.open', 'hud.show', 'hud.hide', 'view.ready',
      'ui.action', 'osfui.handleBack', 'osfui.gamepadRaw', 'log'
    ]);
    if (builtIn.has(command)) {
      reply(message.requestId, 'ui.result', { ok: true, command, message: 'Handled by browser harness' });
      return;
    }
    const error = {
      code: 'mock-unhandled',
      command,
      message: 'No browser-harness response is configured for "' + command + '".'
    };
    if (message.requestId) reply(message.requestId, 'ui.error', error);
    else report('in', { type: 'ui.error', payload: error }, 'warn');
  };

  if (__OSFUI_HARNESS_META__.nativeBridge) {
    window.osfui = window.osfui || {};
    window.osfui.postMessage = postMessage;
  }
  // Vite's development client needs WebSocket for HMR. Network egress remains
  // constrained by the harness CSP and is checked again by the check command.
  for (const name of ['RTCPeerConnection', 'webkitRTCPeerConnection',
    'WebTransport', 'Worker', 'SharedWorker']) {
    try { Object.defineProperty(window, name, { value: undefined, writable: false, configurable: false }); }
    catch {}
  }
  window.addEventListener('message', (event) => {
    if (event.origin !== location.origin || !event.data || event.data.source !== SOURCE) return;
    if (event.data.kind === 'deliver') {
      deliver(event.data.message);
      return;
    }
    if (event.data.kind === 'control' && event.data.action === 'locale' &&
        __OSFUI_HARNESS_META__.nativeBridge) {
      const locale = String(event.data.locale || 'en');
      fixture.locale = locale;
      const message = {
        type: 'i18n.data',
        payload: {
          mod: __OSFUI_HARNESS_META__.modId,
          locale,
          strings: fixture.locales?.[locale] || {},
        },
      };
      report('in', message);
      deliver(message);
    }
  });
  window.addEventListener('DOMContentLoaded', async () => {
    try {
      fixture = await fetch('/__osfui/fixture.json', { cache: 'no-store' }).then((r) => r.json());
    } catch {}
    if (fixture.$error) report('in', fixture.$error, 'warn');
    if (!__OSFUI_HARNESS_META__.nativeBridge) {
      parent.postMessage({ source: SOURCE, kind: 'ready' }, location.origin);
      return;
    }
    const timer = setInterval(() => {
      if (!flush()) return;
      clearInterval(timer);
      const ready = {
        type: 'runtime.ready',
        payload: { game: 'Starfield', plugin: 'OSF UI Harness', version: __OSFUI_HARNESS_META__.version,
          bridgeVersion: __OSFUI_HARNESS_META__.bridgeVersion }
      };
      report('in', ready);
      deliver(ready);
      for (const [key, value] of Object.entries(fixture.state || {})) {
        const state = { type: 'data.state', payload: { mod: __OSFUI_HARNESS_META__.modId, key, value } };
        report('in', state);
        deliver(state);
      }
      parent.postMessage({ source: SOURCE, kind: 'ready' }, location.origin);
    }, 0);
    setTimeout(() => clearInterval(timer), 5000);
  });
})();
`;
