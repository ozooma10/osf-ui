// Authoring-harness shell page logic (/__osfui/harness.js). Loaded as a
// module script by HARNESS_HTML; talks to the view iframe over postMessage
// envelopes tagged source:'osfui-harness'.

import { STAGE_MODES, computeFit, nextStageMode } from './stage-fit.js';
import { applyPatch, nextCycleValue } from './tools-model.js';

const $ = (id) => document.getElementById(id);
const frame = $('view');
const stage = $('stage');
const shell = $('stage-shell');
const traffic = $('traffic');
let meta;
let visible = true;
let checker = true;

// ?res=fixed|fill|off preselects the stage mode, mirroring the in-page cycle
// button. 'fixed' is the authoring baseline.
const params = new URLSearchParams(location.search);
let stageMode = STAGE_MODES.includes(params.get('res')) ? params.get('res') : 'fixed';

/** Button face and tooltip per stage mode, in cycle order. */
const STAGE_LABELS = {
  fixed: {
    label: () => $('width').value + '×' + $('height').value,
    title: 'Stage: the declared reference frame, letterboxed and scaled to the pane. Click to fill the pane instead.',
  },
  fill: {
    label: () => 'Fill pane',
    title: 'Stage: the reference row height, widened to the pane aspect — how the game resizes the view to the output. Click for fluid (unscaled) mode.',
  },
  off: {
    label: () => 'Fluid',
    title: 'No stage: the view reflows to the raw pane, unscaled. Click to return to the reference frame.',
  },
};

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
  requestAnimationFrame(scaleStage);
}

function scaleStage() {
  if (!meta) return;
  const mode = STAGE_LABELS[stageMode] ? stageMode : 'fixed';
  $('stage-mode').textContent = STAGE_LABELS[mode].label();
  $('stage-mode').title = STAGE_LABELS[mode].title;
  if (mode === 'off') {
    // Fluid: the iframe reflows to the raw pane, no transform. For inspecting
    // overflow, not for authoring layout.
    stage.style.width = '100%';
    stage.style.height = '100%';
    stage.style.transform = 'none';
    stage.style.margin = '0';
    $('status').textContent = 'fluid';
    return;
  }
  const width = Number($('width').value);
  const height = Number($('height').value);
  const availableWidth = Math.max(1, shell.clientWidth - 48);
  const availableHeight = Math.max(1, shell.clientHeight - 48);
  const fit = computeFit(availableWidth, availableHeight, width, height, mode);
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

async function loadMeta(navigate = true) {
  meta = await fetch('/__osfui/meta.json', { cache: 'no-store' }).then((r) => r.json());
  $('view-id').textContent = meta.qualifiedId + ' — ' + (meta.title || meta.qualifiedId);
  setSize(meta.width, meta.height);
  if (navigate) frame.src = meta.viewUrl;
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
