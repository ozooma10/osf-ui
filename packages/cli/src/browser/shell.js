// Authoring-harness shell page logic (/__osfui/harness.js). Loaded as a
// module script by HARNESS_HTML; talks to the view iframe over postMessage
// envelopes tagged source:'osfui-harness'.

import { STAGE_MODES, computeFit, nextStageMode } from './stage-fit.js';
import { applyPatch, nextCycleValue } from './tools-model.js';
import { summarize } from './traffic-model.js';

const $ = (id) => document.getElementById(id);
const frame = $('view');
const stage = $('stage');
const shell = $('stage-shell');
const traffic = $('traffic');
let meta;
let visible = true;
let checker = true;

// ?res=fill|off preselects the stage mode, mirroring the in-page cycle button.
// 'fill' is the authoring baseline.
const params = new URLSearchParams(location.search);
let stageMode = STAGE_MODES.includes(params.get('res')) ? params.get('res') : 'fill';

/** Button face and tooltip per stage mode, in cycle order. */
const STAGE_LABELS = {
  fill: {
    label: () => 'Fill pane',
    title: 'Stage: the reference row height, widened to the pane aspect — how the game resizes the view to the output. Click to drop the stage and render unscaled.',
  },
  off: {
    label: () => 'Unscaled',
    title: 'No stage: the view reflows to the raw pane at 1:1 CSS pixels, no scale transform. For inspecting overflow and measuring in DevTools — not a preview of the game. Click to return to the staged frame.',
  },
};

// --- Bridge traffic panel -------------------------------------------------
// A row is a scannable headline (see traffic-model.js) that expands to the
// raw envelope on click. Identical consecutive rows collapse to a ×N counter,
// requests are tagged and paired with their reply (with the round trip in ms),
// and the filter box / pause button make a busy stream readable.

const MAX_ROWS = 300;
const DIRECTIONS = {
  out: { glyph: '▲', title: 'view → native' },
  in: { glyph: '▼', title: 'native → view' },
};

let paused = false;
let held = [];          // entries that arrived while paused
let filterText = '';
let lastRow = null;     // { key, count, badge, time } of the row at the bottom
const requests = new Map(); // requestId -> { tag, sent }
let requestSeq = 0;

function log(direction, value, level = '') {
  const entry = { direction, value, level, at: new Date() };
  if (paused) {
    held.push(entry);
    if (held.length > MAX_ROWS) held.shift();
    renderPause();
    return;
  }
  addRow(entry);
}

function chip(className, text, title = '') {
  const span = document.createElement('span');
  span.className = className;
  span.textContent = text;
  if (title) span.title = title;
  return span;
}

function matchesFilter(row) {
  return !filterText || row.dataset.search.includes(filterText);
}

function addRow({ direction, value, level, at }) {
  const info = summarize(direction, value, level);
  const stamp = at.toLocaleTimeString();

  // Repeats (state pushes, gamepad spam) fold into the previous row. Anything
  // carrying a requestId is a distinct exchange and always gets its own.
  if (lastRow && lastRow.key === info.key && !info.requestId) {
    lastRow.count += 1;
    lastRow.badge.hidden = false;
    lastRow.badge.textContent = '×' + lastRow.count;
    lastRow.time.textContent = stamp;
    return;
  }

  const atBottom = traffic.scrollHeight - traffic.scrollTop - traffic.clientHeight < 32;
  const row = document.createElement('li');
  row.className = 'row ' + info.tone;

  const head = document.createElement('button');
  head.type = 'button';
  head.className = 'row-head';
  const time = chip('time', stamp);
  const arrow = DIRECTIONS[direction] || DIRECTIONS.in;
  head.append(time, chip('dir', arrow.glyph, arrow.title), chip('title', info.title));
  if (info.detail) head.append(chip('detail', info.detail, info.detail));

  // Request tagging: the outbound command mints the tag, its reply reuses it
  // and reports the round trip, so a slow or missing answer is visible.
  if (info.requestId) {
    const open = requests.get(info.requestId);
    if (open && direction === 'in') {
      requests.delete(info.requestId);
      head.append(chip('tag', open.tag + ' ' + Math.round(performance.now() - open.sent) + 'ms',
        'Reply to request ' + open.tag));
    } else {
      const tag = open ? open.tag : '#' + ++requestSeq;
      requests.set(info.requestId, { tag, sent: performance.now() });
      // Unanswered requests would otherwise accumulate for the session.
      if (requests.size > MAX_ROWS) requests.delete(requests.keys().next().value);
      head.append(chip('tag', tag, 'Request ' + tag + ' — awaiting a reply'));
    }
  }
  const badge = chip('count', '');
  badge.hidden = true;
  head.append(badge);

  row.append(head);
  if (info.body) {
    const body = document.createElement('pre');
    body.className = 'row-body';
    body.textContent = info.body;
    body.hidden = true;
    head.addEventListener('click', () => {
      body.hidden = !body.hidden;
      row.classList.toggle('open', !body.hidden);
    });
    row.append(body);
  } else {
    head.disabled = true;
  }

  row.dataset.search = (info.title + ' ' + info.detail + ' ' + info.body).toLowerCase();
  row.hidden = !matchesFilter(row);
  traffic.append(row);
  lastRow = { key: info.key, count: 1, badge, time };
  while (traffic.children.length > MAX_ROWS) traffic.firstElementChild.remove();
  if (atBottom) traffic.scrollTop = traffic.scrollHeight;
}

function renderPause() {
  $('traffic-pause').textContent = paused
    ? 'Paused' + (held.length ? ' (' + held.length + ')' : '')
    : 'Pause';
  $('traffic-pause').classList.toggle('on', paused);
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
  requestAnimationFrame(scaleStage);
}

function scaleStage() {
  if (!meta) return;
  const mode = STAGE_LABELS[stageMode] ? stageMode : 'fill';
  $('stage-mode').textContent = STAGE_LABELS[mode].label();
  $('stage-mode').title = STAGE_LABELS[mode].title;
  if (mode === 'off') {
    // Unscaled: the iframe reflows to the raw pane, no transform. For
    // inspecting overflow, not for authoring layout.
    stage.style.width = '100%';
    stage.style.height = '100%';
    stage.style.transform = 'none';
    stage.style.margin = '0';
    $('status').textContent = 'unscaled';
    return;
  }
  // Only the declared height enters the fit: it pins the scale, and the stage
  // width follows the pane the way the game widens the page to the output.
  const height = Number($('height').value);
  const availableWidth = Math.max(1, shell.clientWidth - 48);
  const availableHeight = Math.max(1, shell.clientHeight - 48);
  const fit = computeFit(availableWidth, availableHeight, height);
  stage.style.width = fit.width + 'px';
  stage.style.height = fit.height + 'px';
  stage.style.transform = 'scale(' + fit.scale + ')';
  // The transform does not affect layout; margins re-centre the scaled box
  // inside the pane (negative when scaled down, positive when scaled up).
  stage.style.margin = ((fit.height * fit.scale - fit.height) / 2) + 'px ' +
    ((fit.width * fit.scale - fit.width) / 2) + 'px';
  $('status').textContent = Math.round(fit.width) + '×' + Math.round(fit.height) +
    ' at ' + Math.round(fit.scale * 100) + '%';
}

// Mock-registered dev controls (postMessage kinds tools/tool-state/tool-invoke).
let tools = [];

function invokeTool(id, value) {
  frame.contentWindow?.postMessage(
    { source: 'osfui-harness', kind: 'tool-invoke', id, value },
    location.origin,
  );
}

function renderTools() {
  const strip = $('tools');
  strip.replaceChildren();
  for (const tool of tools) {
    if (tool.kind === 'select') {
      const label = document.createElement('label');
      label.append(tool.label + ' ');
      const select = document.createElement('select');
      for (const option of tool.options) {
        const element = document.createElement('option');
        element.value = option.value;
        element.textContent = option.label;
        if (option.value === tool.value) element.selected = true;
        select.append(element);
      }
      select.title = tool.title;
      select.addEventListener('change', () => {
        tools = applyPatch(tools, tool.id, { value: select.value });
        invokeTool(tool.id, select.value);
      });
      label.append(select);
      strip.append(label);
      continue;
    }
    const button = document.createElement('button');
    button.type = 'button';
    button.title = tool.title;
    button.classList.toggle('on', tool.active || tool.value === true);
    if (tool.kind === 'toggle') {
      button.textContent = tool.label + ': ' + (tool.value ? 'on' : 'off');
      button.addEventListener('click', () => {
        const next = !tools.find((entry) => entry.id === tool.id)?.value;
        tools = applyPatch(tools, tool.id, { value: next });
        renderTools();
        invokeTool(tool.id, next);
      });
    } else if (tool.kind === 'cycle') {
      const current = tool.options.find((option) => option.value === tool.value);
      button.textContent = tool.label + ': ' + (current ? current.label : tool.value);
      button.addEventListener('click', () => {
        const live = tools.find((entry) => entry.id === tool.id) || tool;
        const next = nextCycleValue(live);
        tools = applyPatch(tools, tool.id, { value: next });
        renderTools();
        invokeTool(tool.id, next);
      });
    } else {
      button.textContent = tool.label;
      button.addEventListener('click', () => invokeTool(tool.id));
    }
    strip.append(button);
  }
}

let views = [];

/**
 * The iframe URL: the view entry plus every shell query param that is not
 * shell-owned, and the hash. That is how ?locale=, ?scenario=, ?health= and
 * #mod= deep links documented for a mock reach it.
 */
function viewSrc(target) {
  const forwarded = new URLSearchParams(location.search);
  for (const own of ['view', 'res', 'checker']) forwarded.delete(own);
  const query = forwarded.toString();
  return target.viewUrl + (query ? '?' + query : '') + location.hash;
}

function selectView(qualifiedId, navigate = true) {
  meta = views.find((entry) => entry.qualifiedId === qualifiedId) || views[0];
  if (!meta) return;
  $('view-select').value = meta.qualifiedId;
  $('view-id').textContent = meta.title && meta.title !== meta.qualifiedId ? meta.title : '';
  setSize(meta.width, meta.height);
  if (navigate) frame.src = viewSrc(meta);
}

async function loadMeta() {
  const listing = await fetch('/__osfui/meta.json', { cache: 'no-store' }).then((r) => r.json());
  views = listing.views;
  const picker = $('view-select');
  picker.replaceChildren();
  for (const entry of views) {
    const option = document.createElement('option');
    option.value = entry.qualifiedId;
    option.textContent = entry.qualifiedId;
    picker.append(option);
  }
  picker.addEventListener('change', () => selectView(picker.value));
  selectView(new URLSearchParams(location.search).get('view') || listing.initial);
}

window.addEventListener('message', (event) => {
  if (event.origin !== location.origin || !event.data || event.data.source !== 'osfui-harness') return;
  if (event.data.kind === 'traffic') log(event.data.direction, event.data.message, event.data.level);
  if (event.data.kind === 'mock-status') {
    log('in', (event.data.ok ? 'Mock: ' : 'Mock failed: ') + event.data.message, event.data.ok ? '' : 'warn');
  }
  if (event.data.kind === 'tools') {
    tools = Array.isArray(event.data.tools) ? event.data.tools : [];
    renderTools();
  }
  if (event.data.kind === 'tool-state') {
    tools = applyPatch(tools, event.data.id, event.data.patch);
    renderTools();
  }
  if (event.data.kind === 'ready') {
    $('status').textContent = meta.nativeBridge ? 'Bridge ready' : 'Bridge disabled by manifest';
    if (meta.nativeBridge) {
      send({ type: 'ui.visibility', payload: { visible, reason: 'overlay' } });
    }
  }
});
window.addEventListener('resize', scaleStage);

$('apply-size').addEventListener('click', () => setSize($('width').value, $('height').value));
$('stage-mode').addEventListener('click', () => {
  stageMode = nextStageMode(stageMode);
  scaleStage();
});
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
// Message injectors for pushes the runtime sends in game. The release edge
// after a gamepad press matters: the kit's button-edge detection reports a
// down edge once per press and needs the up to re-arm.
const PAD_BUTTONS = { LB: 0x0100, RB: 0x0200 };
function injectGamepad(name) {
  const id = PAD_BUTTONS[name];
  send({ type: 'ui.gamepad', payload: { kind: 'button', button: { id, down: true } } });
  setTimeout(() => send({ type: 'ui.gamepad', payload: { kind: 'button', button: { id, down: false } } }), 0);
}
$('inject-hotkey').addEventListener('click', () => {
  const key = $('hotkey-key').value.trim() || 'toggleKey';
  send({ type: 'ui.hotkey', payload: { mod: meta.modId, key } });
});
$('inject-lb').addEventListener('click', () => injectGamepad('LB'));
$('inject-rb').addEventListener('click', () => injectGamepad('RB'));

// Drag-drop: forward .json files into the iframe; the mock decides what they
// are (<modId>_<locale>.json catalogs merge automatically, the rest goes to
// ctx.onDrop).
window.addEventListener('dragover', (event) => event.preventDefault());
window.addEventListener('drop', async (event) => {
  event.preventDefault();
  const files = [...(event.dataTransfer?.files || [])].filter((file) => file.name.endsWith('.json'));
  if (!files.length) return;
  const payload = [];
  for (const file of files) payload.push({ name: file.name, text: await file.text() });
  frame.contentWindow?.postMessage({ source: 'osfui-harness', kind: 'drop', files: payload }, location.origin);
  log('in', 'Dropped: ' + payload.map((file) => file.name).join(', '));
});

$('traffic-filter').addEventListener('input', () => {
  filterText = $('traffic-filter').value.trim().toLowerCase();
  for (const row of traffic.children) row.hidden = !matchesFilter(row);
});
$('traffic-pause').addEventListener('click', () => {
  paused = !paused;
  if (!paused) {
    const pending = held;
    held = [];
    for (const entry of pending) addRow(entry);
  }
  renderPause();
});
$('traffic-clear').addEventListener('click', () => {
  traffic.replaceChildren();
  held = [];
  lastRow = null;
  requests.clear();
  renderPause();
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
