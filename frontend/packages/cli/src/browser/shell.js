const SOURCE = 'osfui-harness';
const frame = document.querySelector('#view');
const stage = document.querySelector('#stage');
const tools = document.querySelector('#tools');
const status = document.querySelector('#status');
const meta = await fetch('/__osfui/meta.json').then((response) => response.json());
const view = meta.views.find((candidate) => candidate.qualifiedId === meta.initial) ?? meta.views[0];

document.querySelector('#view-id').textContent = view.qualifiedId;
stage.style.setProperty('--width', view.width);
stage.style.setProperty('--height', view.height);
frame.src = view.viewUrl;

function fit() {
  const container = stage.parentElement.getBoundingClientRect();
  const scale = Math.min(container.width / view.width, container.height / view.height, 1);
  stage.style.width = `${view.width}px`;
  stage.style.height = `${view.height}px`;
  stage.style.transform = `scale(${scale})`;
}
new ResizeObserver(fit).observe(stage.parentElement);
fit();

function optionsFor(raw) {
  return Array.isArray(raw) ? raw.map((option) => typeof option === 'string'
    ? { value: option, label: option }
    : option).filter((option) => option && typeof option.value === 'string') : [];
}

let currentTools = [];
let runtimeReported = false;

function invoke(id, value) {
  if (value !== undefined) {
    renderTools(currentTools.map((tool) => tool.id === id ? { ...tool, value } : tool));
  }
  frame.contentWindow?.postMessage({
    source: SOURCE,
    kind: 'tool-invoke',
    id,
    ...(value === undefined ? {} : { value }),
  }, location.origin);
}

function renderTools(raw) {
  currentTools = Array.isArray(raw) ? raw : [];
  tools.replaceChildren();
  for (const tool of currentTools) {
    if (!tool || typeof tool.id !== 'string' || typeof tool.label !== 'string') continue;
    const label = document.createElement('label');
    label.classList.toggle('active', tool.active === true);
    label.title = tool.title || '';
    if (tool.kind === 'button') {
      const button = document.createElement('button');
      button.type = 'button';
      button.textContent = tool.label;
      button.addEventListener('click', () => invoke(tool.id));
      label.append(button);
    } else if (tool.kind === 'toggle') {
      const button = document.createElement('button');
      button.type = 'button';
      button.textContent = `${tool.label}: ${tool.value === true ? 'On' : 'Off'}`;
      button.setAttribute('aria-pressed', String(tool.value === true));
      button.addEventListener('click', () => invoke(tool.id, tool.value !== true));
      label.append(button);
    } else if (tool.kind === 'cycle') {
      const choices = optionsFor(tool.options);
      const selected = choices.find((option) => option.value === tool.value) || choices[0];
      if (!selected) continue;
      const button = document.createElement('button');
      button.type = 'button';
      button.textContent = `${tool.label}: ${selected.label || selected.value}`;
      button.addEventListener('click', () => {
        const index = choices.findIndex((option) => option.value === selected.value);
        invoke(tool.id, choices[(index + 1) % choices.length].value);
      });
      label.append(button);
    } else if (tool.kind === 'select') {
      label.append(`${tool.label} `);
      const select = document.createElement('select');
      for (const option of optionsFor(tool.options)) {
        const element = document.createElement('option');
        element.value = option.value;
        element.textContent = option.label || option.value;
        select.append(element);
      }
      select.value = typeof tool.value === 'string' ? tool.value : select.options[0]?.value;
      select.addEventListener('change', () => invoke(tool.id, select.value));
      label.append(select);
    } else {
      continue;
    }
    tools.append(label);
  }
}

function patchTool(id, patch) {
  if (!patch || typeof patch !== 'object') return;
  renderTools(currentTools.map((tool) => tool.id === id ? { ...tool, ...patch } : tool));
}

window.addEventListener('message', (event) => {
  if (event.origin !== location.origin || event.data?.source !== SOURCE) return;
  if (event.data.kind === 'tools') renderTools(event.data.tools);
  if (event.data.kind === 'tool-state') patchTool(event.data.id, event.data.patch);
  if (event.data.kind === 'mock-status') {
    runtimeReported = true;
    status.textContent = event.data.ok ? 'Ready' : `Mock failed: ${event.data.message || 'unknown error'}`;
    status.classList.toggle('error', !event.data.ok);
  }
  if (event.data.kind === 'ready' && !status.classList.contains('error')) status.textContent = 'Ready';
});

frame.addEventListener('load', () => {
  if (!runtimeReported) status.textContent = 'View loaded';
});
document.querySelector('#reload').addEventListener('click', () => frame.contentWindow?.location.reload());
