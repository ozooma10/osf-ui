#!/usr/bin/env node
import { cp, mkdir, readdir, writeFile } from 'node:fs/promises';
import { basename, dirname, resolve } from 'node:path';
import { spawn } from 'node:child_process';
import { fileURLToPath } from 'node:url';
import {
  finishPrompt,
  ID,
  MOD_ID,
  promptMissing,
  PromptCancelledError,
  slug,
} from './prompts.mjs';
import { resolveCliSpec } from './cli-spec.mjs';
import { backendConfig, backendFiles, backendGuide } from './backend-templates.mjs';

const HERE = dirname(fileURLToPath(import.meta.url));

function parse(argv) {
  const result = { _: [] };
  for (let index = 0; index < argv.length; index++) {
    const value = argv[index];
    if (!value.startsWith('--')) result._.push(value);
    else {
      const key = value.slice(2).replace(/-([a-z])/g, (_, letter) => letter.toUpperCase());
      if (['--yes', '--no-install', '--help'].includes(value)) result[key] = true;
      else result[key] = argv[++index];
    }
  }
  return result;
}

function validate(options) {
  if (!MOD_ID.test(options.modId)) throw new Error('--mod-id must be lowercase <author>.<modname>.');
  if (!ID.test(options.view)) throw new Error('--view must use lowercase letters, digits, and hyphens.');
  if (options.template !== undefined && options.template !== 'typescript') {
    throw new Error('--template was removed; projects are TypeScript (plain .js files still build).');
  }
  if (!['menu', 'hud'].includes(options.surface)) throw new Error('--surface must be menu or hud.');
  if (!['papyrus', 'native'].includes(options.integration)) {
    throw new Error('--integration must be papyrus or native.');
  }
}

async function put(root, relative, content) {
  const path = resolve(root, relative);
  await mkdir(resolve(path, '..'), { recursive: true });
  await writeFile(path, content);
}

function hudAppSource(options) {
  const native = options.integration === 'native';
  const stateSetup = native
    ? `type HudState = {
  value: number;
  maximum: number;
  label: string;
  status: string;
  alert: boolean;
};

window.osfui?.on?.<HudState>('${options.modId}.hudState', renderState);`
    : `// Papyrus SetView* values are cached and replayed to late subscribers.
window.osfui?.data?.on<string>('label', (value) => setText(label, value));
window.osfui?.data?.on<number>('value', (value) => {
  hudState.value = value;
  renderMeter();
});
window.osfui?.data?.on<number>('maximum', (value) => {
  hudState.maximum = value;
  renderMeter();
});
window.osfui?.data?.on<string>('status', (value) => setText(status, value));
window.osfui?.data?.on<boolean>('alert', (value) => {
  hudState.alert = value;
  panel.classList.toggle('is-alert', value);
});`;

  return `import './style.css';
import '/shared/osfui.css';
import '/shared/osfui.js';
import type {
  SettingValue,
  SettingsChangedPayload,
  SettingsDataPayload,
  UiHotkeyPayload,
} from '@osfui/cli/view';

type Anchor = 'top-left' | 'top-right' | 'bottom-left' | 'bottom-right';
type HudSettings = {
  hudEnabled: boolean;
  anchor: Anchor;
  margin: number;
  scale: number;
  opacity: number;
  accent: string;
};

const app = document.querySelector('#app');
if (!(app instanceof HTMLElement)) throw new Error('Missing #app element');
app.innerHTML =
  '<main id="hud" class="hud-shell" data-anchor="top-right">' +
    '<section class="hud-panel" role="status" aria-label="System status">' +
      '<header><span id="label">SYSTEM INTEGRITY</span><b id="status">WAITING</b></header>' +
      '<div class="reading"><strong id="value">—</strong><span id="maximum">/ —</span></div>' +
      '<div class="meter" aria-hidden="true"><i id="meter-fill"></i></div>' +
    '</section>' +
  '</main>';

function requiredElement<T extends Element>(selector: string, kind: { new(): T }): T {
  const element = document.querySelector(selector);
  if (!(element instanceof kind)) throw new Error('Missing ' + selector);
  return element;
}

const hud = requiredElement('#hud', HTMLElement);
const panel = requiredElement('.hud-panel', HTMLElement);
const label = requiredElement('#label', HTMLElement);
const status = requiredElement('#status', HTMLElement);
const value = requiredElement('#value', HTMLElement);
const maximum = requiredElement('#maximum', HTMLElement);
const meterFill = requiredElement('#meter-fill', HTMLElement);

const hudState = { value: 0, maximum: 0, alert: false };
const hudSettings: HudSettings = {
  hudEnabled: true,
  anchor: 'top-right',
  margin: 32,
  scale: 100,
  opacity: 0.9,
  accent: '#7bdcff',
};
let hotkeyVisible = true;

function setText(element: HTMLElement, text: unknown) {
  element.textContent = String(text);
}

function renderMeter() {
  const max = Math.max(0, Number(hudState.maximum) || 0);
  const current = Math.max(0, Number(hudState.value) || 0);
  const percent = max > 0 ? Math.min(100, current / max * 100) : 0;
  setText(value, current);
  setText(maximum, '/ ' + max);
  meterFill.style.width = percent + '%';
}

function renderState(state: {
  value: number;
  maximum: number;
  label: string;
  status: string;
  alert: boolean;
}) {
  hudState.value = state.value;
  hudState.maximum = state.maximum;
  hudState.alert = state.alert;
  setText(label, state.label);
  setText(status, state.status);
  panel.classList.toggle('is-alert', state.alert);
  renderMeter();
}

function numberSetting(value: SettingValue, fallback: number): number {
  return typeof value === 'number' && Number.isFinite(value) ? value : fallback;
}

function applySetting(key: string, settingValue: SettingValue) {
  switch (key) {
    case 'hudEnabled':
      if (typeof settingValue === 'boolean') hudSettings.hudEnabled = settingValue;
      syncVisibility();
      break;
    case 'anchor':
      if (typeof settingValue === 'string' &&
          ['top-left', 'top-right', 'bottom-left', 'bottom-right'].includes(settingValue)) {
        hudSettings.anchor = settingValue as Anchor;
        hud.dataset.anchor = hudSettings.anchor;
      }
      break;
    case 'margin':
      hudSettings.margin = numberSetting(settingValue, hudSettings.margin);
      hud.style.setProperty('--hud-margin', hudSettings.margin + 'px');
      break;
    case 'scale':
      hudSettings.scale = numberSetting(settingValue, hudSettings.scale);
      hud.style.setProperty('--hud-scale', String(hudSettings.scale / 100));
      break;
    case 'opacity':
      hudSettings.opacity = numberSetting(settingValue, hudSettings.opacity);
      hud.style.setProperty('--hud-opacity', String(hudSettings.opacity));
      break;
    case 'accent':
      if (typeof settingValue === 'string') {
        hudSettings.accent = settingValue;
        hud.style.setProperty('--hud-accent', settingValue);
      }
      break;
  }
}

function syncVisibility() {
  window.osfui?.send?.('setViewHidden', {
    hidden: !hudSettings.hudEnabled || !hotkeyVisible,
  });
}

// Subscribe before requesting the initial registry so no live edit can race us.
window.osfui?.on?.<SettingsDataPayload>('settings.data', (payload) => {
  const own = payload.mods.find((mod) => mod.id === '${options.modId}');
  if (!own) return;
  for (const [key, settingValue] of Object.entries(own.values)) {
    applySetting(key, settingValue);
  }
});
window.osfui?.on?.<SettingsChangedPayload>('settings.changed', (payload) => {
  if (payload.mod === '${options.modId}') applySetting(payload.key, payload.value);
});
window.osfui?.on?.<UiHotkeyPayload>('ui.hotkey', (payload) => {
  if (payload.mod === '${options.modId}' && payload.key === 'toggleHud') {
    hotkeyVisible = !hotkeyVisible;
    syncVisibility();
  }
});
window.osfui?.send?.('settings.get');

${stateSetup}
`;
}

function nativeAppSource(options) {
  const stateType = `${options.modId}.state`;
  const noticeType = `${options.modId}.notice`;

  return `import '/shared/osfui.css';
import '/shared/osfui.js';
import './style.css';

type DemoState = {
  count: number;
  enabled: boolean;
  greeting: string;
  lastAction: string;
  features: string[];
};
type Greeting = {
  message: string;
  receivedFromJs: { name: string; excited: boolean };
  nativeCount: number;
};

const app = document.querySelector('#app');
if (!(app instanceof HTMLElement)) throw new Error('Missing #app element');
// The osf-* classes come from /shared/osfui.css and carry the hover, press,
// focus, and disabled states — plain <button>/<input> get none of them.
app.innerHTML = '<main class="card osf-card"><p class="osf-eyebrow">${options.surface.toUpperCase()} · NATIVE BRIDGE</p>' +
  '<h1>C++ ↔ JavaScript</h1><p>One generated project showing commands, requests, pushes, settings, and callbacks.</p>' +
  '<section class="state"><span class="osf-eyebrow">Native count</span><strong id="count">—</strong>' +
  '<small id="last-action">Waiting for C++ state…</small><small id="features"></small></section>' +
  '<div class="actions"><button class="osf-btn osf-btn--osf-accent" id="increment">Send command to C++</button></div>' +
  '<form id="greeting"><input class="osf-input" id="name" value="Explorer" aria-label="Name">' +
  '<label><input class="osf-flag-box" id="excited" type="checkbox" checked> Enthusiastic</label>' +
  '<button class="osf-btn" type="submit">Call C++ and await reply</button></form>' +
  '<output id="status">Waiting for OSF UI…</output></main>';

function requiredElement<T extends Element>(selector: string, kind: { new(): T }): T {
  const element = document.querySelector(selector);
  if (!(element instanceof kind)) throw new Error('Missing ' + selector);
  return element;
}

const count = requiredElement('#count', HTMLElement);
const lastAction = requiredElement('#last-action', HTMLElement);
const features = requiredElement('#features', HTMLElement);
const increment = requiredElement('#increment', HTMLButtonElement);
const form = requiredElement('#greeting', HTMLFormElement);
const name = requiredElement('#name', HTMLInputElement);
const excited = requiredElement('#excited', HTMLInputElement);
const status = requiredElement('#status', HTMLOutputElement);

function showState(state: DemoState) {
  count.textContent = String(state.count);
  lastAction.textContent = state.lastAction;
  features.textContent = state.features.join(' · ');
  increment.disabled = !state.enabled;
}

// C++ -> JS: subscribe before asking for current state so later pushes cannot race us.
window.osfui?.on?.<DemoState>('${stateType}', showState);
window.osfui?.on?.<{ message: string }>('${noticeType}', (payload) => {
  status.textContent = payload.message;
});

window.osfui?.ready?.then(async (info) => {
  status.textContent = 'Connected to OSF UI ' + info.version;
  try {
    if (!window.osfui?.call) throw new Error('Request API is unavailable');
    showState(await window.osfui.call<DemoState>('${options.modId}.getState'));
  } catch (error) {
    status.textContent = error instanceof Error ? error.message : String(error);
  }
});

// JS -> C++ fire-and-forget; OnIncrement answers by pushing ${stateType}.
increment.addEventListener('click', () => {
  if (!window.osfui?.send?.('${options.modId}.increment', { amount: 1 })) {
    status.textContent = 'OSF UI bridge is unavailable';
  }
});

// JS -> C++ request/response; OSF UI owns the request id and timeout.
form.addEventListener('submit', async (event) => {
  event.preventDefault();
  try {
    if (!window.osfui?.call) throw new Error('Request API is unavailable');
    const reply = await window.osfui.call<Greeting>('${options.modId}.greet', {
      name: name.value,
      excited: excited.checked,
    });
    status.textContent = reply.message + ' Echo: ' + JSON.stringify(reply.receivedFromJs);
  } catch (error) {
    status.textContent = error instanceof Error ? error.message : String(error);
  }
});
`;
}

function hudMockSource(options) {
  const native = options.integration === 'native';
  const publishBody = native
    ? `ctx.send({
      type: '${options.modId}.hudState',
      payload: { ...state },
    });`
    : `for (const [key, value] of Object.entries(state)) {
      ctx.send({
        type: 'data.state',
        payload: { mod: '${options.modId}', key, value },
      });
    }`;

  return `import { defineMock, type MockContext } from '@osfui/cli';

// Browser-only stand-in for the ${native ? 'native plugin' : 'Papyrus quest'}.
// It exercises telemetry, warnings, settings, placement, and the HUD hotkey.
const state = {
  value: 72,
  maximum: 100,
  label: 'SYSTEM INTEGRITY',
  status: 'NOMINAL',
  alert: false,
};
const settings = {
  hudEnabled: true,
  toggleHud: 'F8',
  anchor: 'top-right',
  margin: 32,
  scale: 100,
  opacity: 0.9,
  accent: '#7bdcff',
};

export default defineMock({
  locales: { en: { title: 'HUD example' } },
  ${native ? '' : 'state,'}
});

export function install(ctx: MockContext) {
  const publishState = () => {
    ${publishBody}
  };
  const publishSettings = () => ctx.send({
    type: 'settings.data',
    payload: {
      mods: [{
        id: '${options.modId}',
        title: '${options.modId}',
        schema: { id: '${options.modId}', title: '${options.modId}', version: 1, groups: [] },
        values: { ...settings },
      }],
    },
  });
  const changeSetting = (key: keyof typeof settings, value: string | number | boolean) => {
    (settings[key] as string | number | boolean) = value;
    ctx.send({
      type: 'settings.changed',
      payload: { mod: '${options.modId}', key, value },
    });
  };

  ctx.onCommand((command) => {
    if (command === 'settings.get') {
      publishSettings();
      return true;
    }
    if (command === 'setViewHidden') {
      // The real host applies this to the WebView layer. The harness toolbar
      // already owns preview visibility, so acknowledging is sufficient here.
      return true;
    }
  });

  ctx.registerTools([
    { id: 'hud-value', kind: 'button', label: 'Change telemetry' },
    { id: 'hud-alert', kind: 'toggle', label: 'Alert', value: false },
    { id: 'hud-enabled', kind: 'toggle', label: 'HUD enabled', value: true },
    {
      id: 'hud-anchor', kind: 'cycle', label: 'Anchor', value: 'top-right',
      options: ['top-left', 'top-right', 'bottom-left', 'bottom-right'],
    },
    {
      id: 'hud-scale', kind: 'cycle', label: 'Scale', value: '100',
      options: ['75', '100', '125', '150'],
    },
    {
      id: 'hud-opacity', kind: 'cycle', label: 'Opacity', value: '0.9',
      options: ['0.4', '0.7', '0.9', '1'],
    },
    {
      id: 'hud-accent', kind: 'cycle', label: 'Accent', value: '#7bdcff',
      options: ['#7bdcff', '#ffb86b', '#ff637d', '#9cff8f'],
    },
    { id: 'hud-hotkey', kind: 'button', label: 'Press F8' },
  ], (id, toolValue) => {
    if (id === 'hud-value') {
      state.value = state.value >= state.maximum ? 12 : state.value + 7;
      publishState();
    } else if (id === 'hud-alert') {
      state.alert = toolValue === true;
      state.status = state.alert ? 'WARNING' : 'NOMINAL';
      publishState();
    } else if (id === 'hud-enabled') {
      changeSetting('hudEnabled', toolValue === true);
    } else if (id === 'hud-anchor' && typeof toolValue === 'string') {
      changeSetting('anchor', toolValue);
    } else if (id === 'hud-scale' && typeof toolValue === 'string') {
      changeSetting('scale', Number(toolValue));
    } else if (id === 'hud-opacity' && typeof toolValue === 'string') {
      changeSetting('opacity', Number(toolValue));
    } else if (id === 'hud-accent' && typeof toolValue === 'string') {
      changeSetting('accent', toolValue);
    } else if (id === 'hud-hotkey') {
      ctx.send({
        type: 'ui.hotkey',
        payload: { mod: '${options.modId}', key: 'toggleHud' },
      });
    }
  });

  // install() runs before the view module; defer the native-style initial push
  // so its osfui.on listener is in place. Papyrus state is also replayed by
  // defineMock, and this explicit publish mirrors a save-load republish.
  setTimeout(publishState, 0);
}
`;
}

function mockSource(options) {
  const mockImport = "import { defineMock, type MockContext } from '@osfui/cli';";
  if (options.surface === 'hud') return hudMockSource(options);
  if (options.integration === 'native') return `${mockImport}

// The browser harness mirrors native/src/main.cpp so every round trip works
// without launching Starfield. This file stays at project root and never ships.
const state = {
  count: 0,
  enabled: true,
  greeting: 'Hello from the mocked C++ plugin',
  lastAction: 'Browser mock initialized',
  features: ['typed JSON', 'commands', 'requests', 'native pushes', 'settings', 'hotkeys'],
};

export default defineMock({
  locales: { en: { title: 'Native bridge example' } },
});

export function install(ctx: MockContext) {
  const pushState = () => ctx.send({
    type: '${options.modId}.state',
    payload: { ...state, features: [...state.features] },
  });
  const notice = (message: string) => ctx.send({
    type: '${options.modId}.notice', payload: { message },
  });

  ctx.onCommand((command, payload, reply) => {
    if (command === '${options.modId}.getState') {
      reply('${options.modId}.state', { ...state, features: [...state.features] });
      return true;
    }
    if (command === '${options.modId}.increment') {
      const requested = Number(payload.amount);
      const amount = Number.isFinite(requested) ? Math.max(-10, Math.min(10, requested)) : 1;
      if (state.enabled) {
        state.count += amount;
        state.lastAction = 'JavaScript sent a fire-and-forget command';
        pushState();
      } else {
        notice('The native counter is disabled in Mod Settings');
      }
      return true;
    }
    if (command === '${options.modId}.greet') {
      const name = typeof payload.name === 'string' ? payload.name : '';
      if (!name) {
        reply('ui.error', { code: 'invalid-payload', message: 'name is required' });
        return true;
      }
      const excited = payload.excited === true;
      reply('${options.modId}.greeting', {
        message: state.greeting + ', ' + name + (excited ? '!!' : '!'),
        receivedFromJs: { name, excited },
        nativeCount: state.count,
      });
      return true;
    }
  });

  ctx.registerTools([
    { id: 'native-enabled', kind: 'toggle', label: 'Native enabled', value: true },
    { id: 'native-hotkey', kind: 'button', label: 'Fire hotkey callback' },
  ], (id, value) => {
    if (id === 'native-enabled') {
      state.enabled = value === true;
      state.lastAction = 'Mocked C++ settings callback applied a value';
      pushState();
    } else if (id === 'native-hotkey') {
      state.lastAction = 'Mocked C++ hotkey callback fired';
      pushState();
      notice('The native open-view hotkey fired');
    }
  });
}
`;

  return `${mockImport}

// Browser-side mock served to \`osfui dev\`: it stands in for the Papyrus
// script so every round trip works without launching Starfield. Lives at the
// project root so it can never ship with the views.
const state = { greeting: 'Hello from the mocked Papyrus script', clicks: 0 };

export default defineMock({
  // Mirrors the script's opening OSFUI.SetView* publish; the harness replays
  // these as data.state on every reload, exactly like the real cache.
  state,
  locales: { en: { title: 'Papyrus example' } },
});

export function install(ctx: MockContext) {
  const publish = () => ctx.send({
    type: 'data.state',
    payload: { mod: '${options.modId}', key: 'clicks', value: state.clicks },
  });

  ctx.onCommand((command, payload, reply) => {
    // Papyrus OnOSFUIViewAction(action, args)
    if (command === 'ui.action') {
      const args = Array.isArray(payload.args) ? payload.args : [];
      if (payload.action === 'bump') {
        state.clicks += Number(args[0]) || 1;
        publish();
      } else if (payload.action === 'openSettings') {
        ctx.notify('Papyrus would call OSFUI.OpenMenu()');
      }
      return true;
    }
    // Papyrus OnOSFUIViewRequest(request, args, replyToken)
    if (command === 'ui.papyrusRequest' && payload.request === 'greet') {
      const args = Array.isArray(payload.args) ? payload.args : [];
      const who = String(args[0] ?? '');
      if (!who) {
        // OSFUI.RejectViewRequest(replyToken, code, message)
        reply('ui.error', { code: 'invalid-name', message: 'Type a name first' });
      } else {
        // OSFUI.ReplyViewString(replyToken, value)
        reply('papyrus.result', { value: 'Hello ' + who + ', from the mocked Papyrus script' });
      }
      return true;
    }
  });
}
`;
}

function appSource(options) {
  if (options.surface === 'hud') return hudAppSource(options);
  if (options.integration === 'native') return nativeAppSource(options);

  return `import '/shared/osfui.css';
import '/shared/osfui.js';
import './style.css';

const app = document.querySelector('#app');
if (!(app instanceof HTMLElement)) throw new Error('Missing #app element');
// The osf-* classes come from /shared/osfui.css and carry the hover, press,
// focus, and disabled states — plain <button>/<input> get none of them.
app.innerHTML = '<main class="card osf-card"><p class="osf-eyebrow">${options.surface.toUpperCase()} · PAPYRUS BRIDGE</p>' +
  '<h1>Papyrus ↔ JavaScript</h1><p>Published state, one-way actions, and awaited requests.</p>' +
  '<section class="state"><span class="osf-eyebrow">Clicks</span><strong id="clicks">—</strong>' +
  '<small id="greeting">Waiting for Papyrus state…</small></section>' +
  '<div class="actions"><button class="osf-btn osf-btn--osf-accent" id="bump">Send action to Papyrus</button>' +
  '<button class="osf-btn" id="settings">Open Mod Settings</button></div>' +
  '<form id="greet"><input class="osf-input" id="name" value="Explorer" aria-label="Name">' +
  '<button class="osf-btn" type="submit">Ask Papyrus and await the reply</button></form>' +
  '<output id="status">Waiting for OSF UI…</output></main>';

function requiredElement<T extends Element>(selector: string, kind: { new(): T }): T {
  const element = document.querySelector(selector);
  if (!(element instanceof kind)) throw new Error('Missing ' + selector);
  return element;
}

function describe(error: unknown): string {
  if (!(error instanceof Error)) return String(error);
  // RejectViewRequest(token, code, message) rejects with both halves.
  return 'code' in error ? String(error.code) + ': ' + error.message : error.message;
}

const clicks = requiredElement('#clicks', HTMLElement);
const greeting = requiredElement('#greeting', HTMLElement);
const bump = requiredElement('#bump', HTMLButtonElement);
const settings = requiredElement('#settings', HTMLButtonElement);
const form = requiredElement('#greet', HTMLFormElement);
const name = requiredElement('#name', HTMLInputElement);
const status = requiredElement('#status', HTMLOutputElement);

// Papyrus SetView* -> cached state, replayed whenever this page (re)loads, so
// subscribing at any time still yields the latest value.
window.osfui?.data?.on<number>('clicks', (value) => {
  clicks.textContent = String(value);
});
window.osfui?.data?.on<string>('greeting', (value) => {
  greeting.textContent = value;
});

window.osfui?.ready?.then((info) => {
  status.textContent = 'Connected to OSF UI ' + info.version;
});

// JS -> Papyrus OnOSFUIViewAction: fire-and-forget. The script answers by
// publishing new state, not by replying.
bump.addEventListener('click', () => {
  if (!window.osfui?.action?.('bump', 1)) status.textContent = 'OSF UI bridge is unavailable';
});
settings.addEventListener('click', () => {
  window.osfui?.action?.('openSettings');
});

// JS -> Papyrus OnOSFUIViewRequest: use this only when the returned value is
// the point. OSF UI owns correlation and the ten-second reply token.
form.addEventListener('submit', async (event) => {
  event.preventDefault();
  try {
    if (!window.osfui?.papyrus) throw new Error('OSF UI bridge is unavailable');
    status.textContent = await window.osfui.papyrus.request<string>('greet', name.value);
  } catch (error) {
    status.textContent = describe(error);
  }
});
`;
}

function styleSource(options) {
  if (options.surface === 'hud') {
    return `:root {
  font-family: system-ui, sans-serif;
  color: #eef7fb;
  background: transparent;
}
* { box-sizing: border-box; }
html, body, #app { width: 100%; height: 100%; }
body { margin: 0; overflow: hidden; background: transparent; pointer-events: none; }
.hud-shell {
  --hud-accent: #7bdcff;
  --hud-margin: 32px;
  --hud-opacity: .9;
  --hud-scale: 1;
  position: fixed;
  inset: 0;
  display: flex;
  padding: var(--hud-margin);
  opacity: var(--hud-opacity);
}
.hud-shell[data-anchor="top-left"] { align-items: flex-start; justify-content: flex-start; }
.hud-shell[data-anchor="top-right"] { align-items: flex-start; justify-content: flex-end; }
.hud-shell[data-anchor="bottom-left"] { align-items: flex-end; justify-content: flex-start; }
.hud-shell[data-anchor="bottom-right"] { align-items: flex-end; justify-content: flex-end; }
.hud-panel {
  width: 360px;
  padding: 18px 20px;
  border: 1px solid color-mix(in srgb, var(--hud-accent) 75%, transparent);
  border-left: 4px solid var(--hud-accent);
  background: linear-gradient(110deg, rgba(6, 16, 23, .94), rgba(6, 16, 23, .7));
  box-shadow: 0 10px 38px rgba(0, 0, 0, .35);
  transform: scale(var(--hud-scale));
}
[data-anchor$="left"] .hud-panel { transform-origin: left center; }
[data-anchor$="right"] .hud-panel { transform-origin: right center; }
.hud-panel header { display: flex; justify-content: space-between; gap: 16px; }
.hud-panel header span { color: var(--hud-accent); font-size: 13px; letter-spacing: .15em; }
.hud-panel header b { font-size: 12px; letter-spacing: .12em; }
.reading { display: flex; align-items: baseline; gap: 7px; margin: 12px 0 10px; }
.reading strong { font-size: 48px; line-height: 1; font-weight: 500; }
.reading span { color: #a9bdc8; font-size: 18px; }
.meter { height: 5px; overflow: hidden; background: rgba(255, 255, 255, .12); }
.meter i {
  display: block;
  width: 0;
  height: 100%;
  background: var(--hud-accent);
  transition: width 180ms ease-out;
}
.hud-panel.is-alert {
  --hud-accent: #ff637d;
  animation: hud-alert 900ms ease-in-out infinite alternate;
}
@keyframes hud-alert { to { box-shadow: 0 0 22px color-mix(in srgb, var(--hud-accent) 35%, transparent); } }
@media (prefers-reduced-motion: reduce) {
  .meter i { transition: none; }
  .hud-panel.is-alert { animation: none; }
}
`;
  }

  return `/* View-specific LAYOUT only. The look — palette, type, and every
   interactive state (hover, press, focus, disabled) — comes from
   /shared/osfui.css, which main.ts imports before this file. Reach for
   the osf-* kit classes (osf-card, osf-btn, osf-input, osf-eyebrow)
   instead of restyling bare elements: a local "button { background: ... }"
   silently overrides the kit and leaves the control feeling dead. */
body { min-height: 100vh; display: grid; place-items: center; padding: 24px; }
.card { width: min(640px, 86vw); padding: 32px; }
.card h1 { margin: 6px 0 8px; font-size: var(--osf-text-3xl); }
.card > p { margin: 0; color: var(--osf-text-muted); }
.state {
  display: grid;
  gap: 6px;
  margin: 22px 0;
  padding: 18px;
  background: var(--osf-accent-quiet);
  border: 1px solid var(--osf-line);
}
.state strong {
  font-family: var(--osf-font-display);
  font-size: 42px;
  line-height: 1;
  color: var(--osf-text-bright);
}
.state small { color: var(--osf-accent-hover); }
.actions, form { display: flex; align-items: center; flex-wrap: wrap; gap: 10px; margin-top: 12px; }
label { display: flex; align-items: center; gap: 8px; color: var(--osf-text-muted); }
output {
  display: block;
  min-height: 24px;
  margin-top: 18px;
  font-family: var(--osf-font-mono);
  font-size: var(--osf-text-xs);
  color: var(--osf-text-muted);
  overflow-wrap: anywhere;
}
`;
}

async function scaffold(options) {
  const root = resolve(options.directory);
  await mkdir(root, { recursive: true });
  if ((await readdir(root)).length) throw new Error(`Directory is not empty: ${root}`);
  const viewRoot = `src/views/${options.modId}/${options.view}`;
  const cliSpec = await resolveCliSpec(root, options.cliSpec);
  const scripts = {
    dev: 'osfui dev',
    'dev:game': 'osfui dev --game',
    check: 'osfui check',
    build: 'osfui build',
    package: 'osfui package',
    doctor: 'osfui doctor',
  };
  if (options.integration === 'native') {
    scripts['build:native'] = 'node native/build.mjs';
    scripts.build = 'npm run build:native && osfui build';
    scripts.package = 'npm run build:native && osfui package';
  }
  const packageJson = {
    name: slug(basename(root)),
    version: '0.1.0',
    private: true,
    type: 'module',
    scripts,
    devDependencies: { '@osfui/cli': cliSpec },
  };
  await put(root, 'package.json', `${JSON.stringify(packageJson, null, 2)}\n`);
  await put(root, '.gitignore', [
    'node_modules/',
    'dist/',
    'release/',
    '.osfui/',
    ...(options.integration === 'papyrus'
      ? ['mod/*.esm', 'mod/*.esp', 'mod/*.esl', 'mod/Scripts/**/*.pex', 'tools/Spriggit.CLI.exe']
      : []),
    '',
  ].join('\n'));
  await put(root, 'tsconfig.json', `${JSON.stringify({
    compilerOptions: {
      target: 'ES2022',
      module: 'ESNext',
      moduleResolution: 'Bundler',
      strict: true,
      // Plain .js view modules build and type-check alongside the .ts ones.
      allowJs: true,
      // DOM: the mock module runs in the browser (osfui check type-checks it).
      lib: ['ES2022', 'DOM', 'DOM.Iterable'],
      types: ['@osfui/cli/view'],
      noEmit: true,
    },
    include: ['src', 'osfui.config.ts', 'osfui.mock.ts'],
  }, null, 2)}\n`);
  await put(root, 'src/vite-env.d.ts', `declare module '*.css';
declare module '*osfui.js';
`);
  for (const file of backendFiles(options)) {
    await put(root, file.path, file.content);
  }
  if (options.integration === 'native') {
    const includeRoot = resolve(root, 'native/include');
    await mkdir(includeRoot, { recursive: true });
    for (const name of ['OSFUI_API.h', 'OSFUI_JSON.h']) {
      await cp(resolve(HERE, '..', `templates/native/${name}`), resolve(includeRoot, name));
    }
  } else {
    const papyrusRoot = resolve(root, 'tools/papyrus');
    await mkdir(papyrusRoot, { recursive: true });
    await cp(
      resolve(HERE, '..', 'templates/papyrus/OSFUI.psc'),
      resolve(papyrusRoot, 'OSFUI.psc'),
    );
  }
  await put(root, `osfui.config.ts`, `import { defineConfig } from '@osfui/cli';

export default defineConfig({
  modId: '${options.modId}',
${backendConfig(options)}  views: [{
    id: '${options.view}',
    title: '${options.view.replaceAll('-', ' ')}',
    kind: '${options.surface}',
    width: ${options.surface === 'hud' ? 1920 : 1200},
    height: ${options.surface === 'hud' ? 1080 : 720},
    transparent: true,
${options.surface === 'hud' ? `    openOnStart: true,
    order: 0,
` : ''}
    permissions: { nativeBridge: true },
  }],
});
`);
  await put(root, `osfui.mock.ts`, mockSource(options));
  await put(root, `${viewRoot}/index.html`, `<!doctype html>
<html lang="en"><head><meta charset="UTF-8"><meta name="viewport" content="width=device-width,initial-scale=1">
<title>${options.view}</title></head><body><div id="app"></div><script type="module" src="./main.ts"></script></body></html>
`);
  await put(root, `${viewRoot}/main.ts`, appSource(options));
  await put(root, `${viewRoot}/style.css`, styleSource(options));
  await put(root, 'README.md', `# ${packageJson.name}

Run \`npm run dev\` for instant browser HMR. Run \`npm run dev:game -- --deploy "path-to-MO2-mods"\`
to create this mod's folder under MO2 and sync into Starfield with temporary
author mode, F11 reload, and F12 DevTools.

Use \`npm run package\` to create a release-ready zip. Files under \`mod/\`
are copied into the mod archive beside the generated view.

${backendGuide(options)}
`);
  return root;
}

function install(root) {
  return new Promise((resolvePromise, reject) => {
    const executable = process.platform === 'win32' ? process.env.ComSpec || 'cmd.exe' : 'npm';
    const args = process.platform === 'win32' ? ['/d', '/s', '/c', 'npm install'] : ['install'];
    const child = spawn(executable, args, { cwd: root, stdio: 'inherit' });
    child.once('exit', (code) => code === 0 ? resolvePromise() : reject(new Error(`npm install exited with ${code}`)));
  });
}

async function main() {
  const options = parse(process.argv.slice(2));
  if (options.help) {
    console.log('npm create osfui@latest [directory] [-- --mod-id author.mod --view main --surface menu --integration papyrus]');
    return;
  }
  options.directory = options._[0];
  const interactive = await promptMissing(options);
  validate(options);
  const root = await scaffold(options);
  if (!options.noInstall) await install(root);
  const firstCommands = options.integration === 'papyrus'
    ? 'npm run doctor\n  npm run dev'
    : 'npm run dev';
  const next = root === process.cwd()
    ? firstCommands
    : `cd ${options.directory}\n  ${firstCommands}`;
  const result = `Created ${root}\n\nNext:\n  ${next}`;
  if (interactive) finishPrompt(result);
  else console.log(`\n${result}`);
}

main().catch((error) => {
  if (error instanceof PromptCancelledError) return;
  console.error(`create-osfui: ${error.message}`);
  process.exitCode = 1;
});
