import '/shared/osfui.css';
import '/shared/osfui.js';
import './style.css';
import type { OSFUIHelper, PlatformEvents, SettingValue, SettingsData } from '@osfui/cli/view';

type DemoState = {
  count: number;
  enabled: boolean;
  greeting: string;
  lastAction: string;
};
type Greeting = {
  message: string;
  receivedFromJs: { name: string; excited: boolean };
  nativeCount: number;
};

const appNode = document.querySelector<HTMLElement>('#app');
if (!appNode) throw new Error('Missing #app element');
const app: HTMLElement = appNode;
// The osf-* classes come from /shared/osfui.css and carry the hover, press,
// focus, and disabled states — plain <button>/<input> get none of them.
app.innerHTML = '<main class="card osf-card"><p class="osf-eyebrow">MENU · NATIVE BRIDGE</p>' +
  '<h1 data-i18n="views.__OSFUI_VIEW_ID__.heading">OSF UI starter</h1>' +
  '<p data-i18n="views.__OSFUI_VIEW_ID__.subtitle">Retained state, one-shot events, one-way sends, and correlated requests.</p>' +
  '<p class="runtime" id="runtime">Bridge setup…</p>' +
  '<section class="state"><span class="osf-eyebrow">Native count</span><strong id="count">—</strong>' +
  '<small id="last-action">Waiting for C++ state…</small></section>' +
  '<div class="actions"><button class="osf-btn osf-btn--osf-accent" id="increment">Send one-way message</button>' +
  '<button class="osf-btn" id="close">Close view</button></div>' +
  '<form id="greeting"><input class="osf-input" id="name" value="Explorer" aria-label="Name">' +
  '<label><input class="osf-flag-box" id="excited" type="checkbox" checked> Enthusiastic</label>' +
  '<button class="osf-btn" type="submit">Await C++ request</button></form>' +
  '<small id="setting">Waiting for settings state…</small>' +
  '<output id="status">Waiting for OSF UI…</output></main>';

function requiredElement<T extends Element>(selector: string, kind: { new(): T }): T {
  const element = document.querySelector(selector);
  if (!(element instanceof kind)) throw new Error('Missing ' + selector);
  return element;
}

const count = requiredElement('#count', HTMLElement);
const lastAction = requiredElement('#last-action', HTMLElement);
const runtime = requiredElement('#runtime', HTMLElement);
const increment = requiredElement('#increment', HTMLButtonElement);
const form = requiredElement('#greeting', HTMLFormElement);
const name = requiredElement('#name', HTMLInputElement);
const excited = requiredElement('#excited', HTMLInputElement);
const setting = requiredElement('#setting', HTMLElement);
const close = requiredElement('#close', HTMLButtonElement);
const status = requiredElement('#status', HTMLOutputElement);
const maybeOsfui = window.osfui as OSFUIHelper | undefined;
if (!maybeOsfui) throw new Error('OSF UI helper is unavailable');
const osfui: OSFUIHelper = maybeOsfui;

function showState(state: DemoState) {
  count.textContent = String(state.count);
  lastAction.textContent = state.lastAction;
  increment.disabled = !state.enabled;
}

function describe(error: unknown): string {
  if (!(error instanceof Error)) return String(error);
  return 'code' in error ? String(error.code) + ': ' + error.message : error.message;
}

// C++ -> JS event: a notice happened once, so it is not replayed after reload.
osfui.on<{ message: string }>('notice', (payload) => {
  status.textContent = payload.message;
});

// The plugin publishes its state with SetViewState, so this needs no read and
// no reload handling: the handler runs with the current value now, and again on
// every change and on every future document. This is the path you want for
// anything the mod backend owns.
osfui.state.on<DemoState>('state', showState);
const initialState = osfui.state.get<DemoState>('state');
if (initialState) showState(initialState); // get() is useful for one-off snapshots; on() is the normal render path.

osfui.ready.then(async (info) => {
  runtime.textContent = info.plugin + ' ' + info.version + ' · bridge ' + info.bridgeVersion +
    ' · ' + info.view + ' · locale ' + osfui.i18n.locale;
  await osfui.i18n.ready;
  osfui.i18n.localize(app);
  status.textContent = osfui.i18n.t(
    'views.__OSFUI_VIEW_ID__.connected', 'Connected to OSF UI {version}', { version: info.version },
  ) + '; osfui.available = ' + osfui.available;
  // A REQUEST is for the other case: a value only this view knows it needs,
  // right now. Here it is redundant with the subscription above — kept as the
  // smallest working example of the verb.
  try {
    showState(await osfui.request<DemoState>('getState'));
  } catch (error) {
    status.textContent = describe(error);
  }
}).catch((error) => { status.textContent = describe(error); });

// JS -> C++ fire-and-forget; OnIncrement answers by publishing retained state.
increment.addEventListener('click', () => {
  if (!osfui.send('increment', { amount: 1 })) {
    status.textContent = 'OSF UI bridge is unavailable';
  }
});

// JS -> C++ request/response; OSF UI owns the correlation id and the timeout,
// and a failure arrives as a rejection carrying a stable code.
form.addEventListener('submit', async (event) => {
  event.preventDefault();
  try {
    const reply = await osfui.request<Greeting>('greet', {
      name: name.value,
      excited: excited.checked,
    });
    status.textContent = reply.message + ' Echo: ' + JSON.stringify(reply.receivedFromJs);
  } catch (error) {
    status.textContent = describe(error);
  }
});

// The settings registry is STATE, so this handler runs immediately with the
// current values and again on every change — no read to issue, no race.
function applySetting(key: string, value: SettingValue) {
  if (key === 'accent' && typeof value === 'string') osfui.theme.applyAccent(app, value);
  const shown = key === 'openKey' && typeof value === 'string'
    ? (value ? keyboardLabels[value] ?? value : 'Unbound')
    : JSON.stringify(value);
  setting.textContent = key + ' = ' + shown;
}
let keyboardLabels: Record<string, string> = {};
osfui.state.on<SettingsData>('osfui/settings', (registry) => {
  // Key values are layout-independent physical names. Show the player's
  // current keycap when the OSF UI runtime publishes one; never store the label.
  keyboardLabels = registry.keyboard?.labels ?? {};
  const own = registry.mods.find((mod) => mod.id === '__OSFUI_MOD_ID_SQ__');
  if (!own) return;
  for (const [key, value] of Object.entries(own.values)) applySetting(key, value);
});
// An individual commit is an EVENT: it says what just changed, so it is never
// replayed (the state key above already carries the current value).
osfui.on<PlatformEvents['settings.changed']>('settings.changed', (payload) => {
  if (payload.mod === '__OSFUI_MOD_ID_SQ__') applySetting(payload.key, payload.value);
});
osfui.on<PlatformEvents['ui.hotkey']>('ui.hotkey', (payload) => {
  if (payload.mod === '__OSFUI_MOD_ID_SQ__') status.textContent = 'Hotkey fired: ' + payload.key;
});

// Own Esc/gamepad-B while active, then close explicitly. Drop these two lines
// if your view wants the default native back behavior.
osfui.send('osfui.handleBack', { handle: true });
window.addEventListener('keydown', (event) => {
  if (event.key === 'Escape') osfui.send('close');
});
close.addEventListener('click', () => osfui.send('close'));
