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

function renderTools(raw) {
  tools.replaceChildren();
  for (const tool of Array.isArray(raw) ? raw : []) {
    if (tool?.kind !== 'select' || typeof tool.id !== 'string') continue;
    const label = document.createElement('label');
    label.textContent = `${tool.label || tool.id} `;
    label.title = tool.title || '';
    const select = document.createElement('select');
    for (const option of optionsFor(tool.options)) {
      const element = document.createElement('option');
      element.value = option.value;
      element.textContent = option.label || option.value;
      select.append(element);
    }
    select.value = typeof tool.value === 'string' ? tool.value : select.options[0]?.value;
    select.addEventListener('change', () => frame.contentWindow?.postMessage({
      source: SOURCE,
      kind: 'tool-invoke',
      id: tool.id,
      value: select.value,
    }, location.origin));
    label.append(select);
    tools.append(label);
  }
}

window.addEventListener('message', (event) => {
  if (event.origin !== location.origin || event.data?.source !== SOURCE) return;
  if (event.data.kind === 'tools') renderTools(event.data.tools);
  if (event.data.kind === 'mock-status') {
    status.textContent = event.data.ok ? 'Ready' : `Mock failed: ${event.data.message || 'unknown error'}`;
    status.classList.toggle('error', !event.data.ok);
  }
});

frame.addEventListener('load', () => { status.textContent = 'View loaded'; });
document.querySelector('#reload').addEventListener('click', () => frame.contentWindow?.location.reload());
