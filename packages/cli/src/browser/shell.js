// Authoring-harness shell page logic (/__osfui/harness.js). Loaded as a
// module script by HARNESS_HTML; talks to the view iframe over postMessage
// envelopes tagged source:'osfui-harness'.

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
