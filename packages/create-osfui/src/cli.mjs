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
import {
  backendConfig,
  backendFiles,
  backendGuide,
  docsGuide,
  pascalIdentifier,
  settingsOnlyFiles,
  settingsOnlyReadme,
  settingsSchema,
} from './backend-templates.mjs';

const HERE = dirname(fileURLToPath(import.meta.url));

// Closed sets: a typo'd flag ("--surfce hud") must fail here, not scaffold
// the default surface and exit 0.
const VALUE_FLAGS = {
  '--mod-id': 'modId',
  '--view': 'view',
  '--surface': 'surface',
  '--integration': 'integration',
  '--cli-spec': 'cliSpec',
};
const BOOLEAN_FLAGS = { '--yes': 'yes', '--no-install': 'noInstall', '--help': 'help' };

function parse(argv) {
  const result = { _: [] };
  for (let index = 0; index < argv.length; index++) {
    const value = argv[index];
    if (!value.startsWith('--')) {
      result._.push(value);
      continue;
    }
    if (BOOLEAN_FLAGS[value]) {
      result[BOOLEAN_FLAGS[value]] = true;
      continue;
    }
    const key = VALUE_FLAGS[value];
    if (!key) {
      throw new Error(`Unknown option "${value}". Known options: ` +
        `${[...Object.keys(VALUE_FLAGS), ...Object.keys(BOOLEAN_FLAGS)].join(', ')}.`);
    }
    const next = argv[++index];
    if (next === undefined || next.startsWith('--')) {
      throw new Error(`Missing value for ${value}.`);
    }
    result[key] = next;
  }
  return result;
}

// Mirrors Ids.h kMaxModIdLen — the native store refuses longer ids at load.
const MAX_MOD_ID_LENGTH = 64;

function validate(options) {
  if (!MOD_ID.test(options.modId)) throw new Error('--mod-id must be lowercase <author>.<modname>.');
  if (options.modId.length > MAX_MOD_ID_LENGTH) {
    throw new Error(`--mod-id must be at most ${MAX_MOD_ID_LENGTH} characters (OSF UI refuses longer ids).`);
  }
  if (!ID.test(options.view)) throw new Error('--view must use lowercase letters, digits, and hyphens.');
  if (!['menu', 'hud', 'settings'].includes(options.surface)) {
    throw new Error('--surface must be menu, hud, or settings.');
  }
  if (!['papyrus', 'native'].includes(options.integration)) {
    throw new Error('--integration must be papyrus or native.');
  }
  // A settings row's only code path back into the game is Papyrus: onPress
  // targets a GLOBAL function, and an action row needs a whole SFSE plugin to
  // answer its request — that is the menu/native preset, not this one.
  if (options.surface === 'settings' && options.integration !== 'papyrus') {
    throw new Error('--surface settings is Papyrus-only; ' +
      'use --surface menu --integration native for an SFSE-plugin project.');
  }
}

function settingsDefaults(options) {
  const values = {};
  for (const group of settingsSchema(options).groups || []) {
    for (const row of group.settings || []) {
      if (typeof row.key === 'string' && Object.hasOwn(row, 'default')) values[row.key] = row.default;
    }
  }
  return values;
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

function renderState(state: HudState) {
  hudState.value = state.value;
  hudState.maximum = state.maximum;
  setText(label, state.label);
  setText(status, state.status);
  panel.classList.toggle('is-alert', state.alert);
  renderMeter();
}

window.osfui?.state?.on?.<HudState>('${options.modId}/hud', renderState);`
    : `// Papyrus SetView* publishes STATE: the handler runs immediately with the
// current value and again on every change and every reload — nothing to
// request, and no ready handshake for the script to answer.
window.osfui?.state?.on?.<string>('${options.modId}/label', (value) => setText(label, value));
window.osfui?.state?.on?.<number>('${options.modId}/value', (value) => {
  hudState.value = value;
  renderMeter();
});
window.osfui?.state?.on?.<number>('${options.modId}/maximum', (value) => {
  hudState.maximum = value;
  renderMeter();
});
window.osfui?.state?.on?.<string>('${options.modId}/status', (value) => setText(status, value));
window.osfui?.state?.on?.<boolean>('${options.modId}/alert', (value) => {
  panel.classList.toggle('is-alert', value);
});`;
  const eventSetup = native
    ? `window.osfui?.on?.<{ message: string }>('${options.modId}.notice', (payload) => {
  setText(notice, payload.message);
});`
    : `window.osfui?.on?.<{ args: string[] }>('${options.modId}.notice', (payload) => {
  setText(notice, payload.args[0] || 'Papyrus event received');
});`;

  return `import '/shared/osfui.css';
import '/shared/osfui.js';
import './style.css';
import type {
  SettingValue,
  PlatformEvents,
  SettingsData,
} from '@osfui/cli/view';

type Anchor = 'top-left' | 'top-right' | 'bottom-left' | 'bottom-right';
type HudSettings = {
  hudEnabled: boolean;
  anchor: Anchor;
  opacity: number;
};

const app = document.querySelector('#app');
if (!(app instanceof HTMLElement)) throw new Error('Missing #app element');
app.innerHTML =
  '<main id="hud" class="hud-shell" data-anchor="top-right">' +
    '<section class="hud-panel" role="status" aria-label="System status">' +
      '<header><span id="label">SYSTEM INTEGRITY</span><b id="status">WAITING</b></header>' +
      '<div class="reading"><strong id="value">—</strong><span id="maximum">/ —</span></div>' +
      '<div class="meter" aria-hidden="true"><i id="meter-fill"></i></div>' +
      '<small id="notice" data-i18n="views.${options.view}.eventHint">Backend events appear here</small>' +
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
const notice = requiredElement('#notice', HTMLElement);

const hudState = { value: 0, maximum: 0 };
const hudSettings: HudSettings = {
  hudEnabled: true,
  anchor: 'top-right',
  opacity: 0.9,
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
    case 'opacity':
      hudSettings.opacity = numberSetting(settingValue, hudSettings.opacity);
      hud.style.setProperty('--hud-opacity', String(hudSettings.opacity));
      break;
  }
}

function syncVisibility() {
  // menu.open / menu.close write the runtime's authoritative shown-set. Do NOT
  // use setViewHidden here: the menu policy rewrites every layer's hidden
  // flag from that set whenever any menu opens or closes, which would undo a
  // raw setViewHidden the moment the player closes the Mods surface.
  const visible = hudSettings.hudEnabled && hotkeyVisible;
  void window.osfui?.request?.(visible ? 'menu.open' : 'menu.close');
}

// The settings registry is STATE: this handler runs immediately with the
// current registry and again on every registry change — including on a fresh
// document, so there is no read to issue and no race to lose.
window.osfui?.state?.on?.<SettingsData>('osfui/settings', (registry) => {
  const own = registry?.mods.find((mod) => mod.id === '${options.modId}');
  if (!own) return;
  for (const [key, settingValue] of Object.entries(own.values)) {
    applySetting(key, settingValue);
  }
});
// Individual commits are EVENTS: they say what just changed, so they are not
// replayed (the state key above already carries the current value).
window.osfui?.on?.<PlatformEvents['settings.changed']>('settings.changed', (payload) => {
  if (payload.mod === '${options.modId}') applySetting(payload.key, payload.value);
});
window.osfui?.on?.<PlatformEvents['ui.hotkey']>('ui.hotkey', (payload) => {
  if (payload.mod === '${options.modId}' && payload.key === 'toggleHud') {
    hotkeyVisible = !hotkeyVisible;
    syncVisibility();
  }
});
${stateSetup}
${eventSetup}
${options.integration === 'papyrus' ? `window.osfui?.papyrus?.call?.('${pascalIdentifier(options.modId)}OSFUI', 'Refresh');` : ''}
window.osfui?.i18n?.ready?.then(() => window.osfui?.i18n?.localize(document));
`;
}

function nativeAppSource(options) {
  const noticeType = `${options.modId}.notice`;

  return `import '/shared/osfui.css';
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
app.innerHTML = '<main class="card osf-card"><p class="osf-eyebrow">${options.surface.toUpperCase()} · NATIVE BRIDGE</p>' +
  '<h1 data-i18n="views.${options.view}.heading">OSF UI starter</h1>' +
  '<p data-i18n="views.${options.view}.subtitle">Retained state, one-shot events, commands, and correlated requests.</p>' +
  '<p class="runtime" id="runtime">Bridge setup…</p>' +
  '<section class="state"><span class="osf-eyebrow">Native count</span><strong id="count">—</strong>' +
  '<small id="last-action">Waiting for C++ state…</small></section>' +
  '<div class="actions"><button class="osf-btn osf-btn--osf-accent" id="increment">Send command</button>' +
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
osfui.on<{ message: string }>('${noticeType}', (payload) => {
  status.textContent = payload.message;
});

// The plugin publishes its state with SetViewState, so this needs no read and
// no reload handling: the handler runs with the current value now, and again on
// every change and on every future document. This is the path you want for
// anything the backend owns.
osfui.state.on<DemoState>('${options.modId}/state', showState);
const initialState = osfui.state.get<DemoState>('${options.modId}/state');
if (initialState) showState(initialState); // get() is useful for one-off snapshots; on() is the normal render path.

osfui.ready.then(async (info) => {
  runtime.textContent = info.plugin + ' ' + info.version + ' · bridge ' + info.bridgeVersion +
    ' · ' + info.view + ' · locale ' + osfui.i18n.locale;
  await osfui.i18n.ready;
  osfui.i18n.localize(app);
  status.textContent = osfui.i18n.t(
    'views.${options.view}.connected', 'Connected to OSF UI {version}', { version: info.version },
  ) + '; osfui.available = ' + osfui.available;
  // A REQUEST is for the other case: a value only this view knows it needs,
  // right now. Here it is redundant with the subscription above — kept as the
  // smallest working example of the verb.
  try {
    showState(await osfui.request<DemoState>('${options.modId}.getState'));
  } catch (error) {
    status.textContent = describe(error);
  }
}).catch((error) => { status.textContent = describe(error); });

// JS -> C++ fire-and-forget; OnIncrement answers by publishing retained state.
increment.addEventListener('click', () => {
  if (!osfui.send('${options.modId}.increment', { amount: 1 })) {
    status.textContent = 'OSF UI bridge is unavailable';
  }
});

// JS -> C++ request/response; OSF UI owns the correlation id and the timeout,
// and a failure arrives as a rejection carrying a stable code.
form.addEventListener('submit', async (event) => {
  event.preventDefault();
  try {
    const reply = await osfui.request<Greeting>('${options.modId}.greet', {
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
  setting.textContent = key + ' = ' + JSON.stringify(value);
}
osfui.state.on<SettingsData>('osfui/settings', (registry) => {
  const own = registry.mods.find((mod) => mod.id === '${options.modId}');
  if (!own) return;
  for (const [key, value] of Object.entries(own.values)) applySetting(key, value);
});
// An individual commit is an EVENT: it says what just changed, so it is never
// replayed (the state key above already carries the current value).
osfui.on<PlatformEvents['settings.changed']>('settings.changed', (payload) => {
  if (payload.mod === '${options.modId}') applySetting(payload.key, payload.value);
});
osfui.on<PlatformEvents['ui.hotkey']>('ui.hotkey', (payload) => {
  if (payload.mod === '${options.modId}') status.textContent = 'Hotkey fired: ' + payload.key;
});

// Own Esc/gamepad-B while active, then close explicitly. Drop these two lines
// if your view wants the default native back behavior.
osfui.send('osfui.handleBack', { handle: true });
window.addEventListener('keydown', (event) => {
  if (event.key === 'Escape') osfui.send('close');
});
close.addEventListener('click', () => osfui.send('close'));

// The manifest opts into readySignal, so first reveal waits for meaningful DOM.
osfui.markReady();
`;
}

function hudMockSource(options) {
  const native = options.integration === 'native';
  const publishBody = native
    ? `ctx.send({
      kind: 'state',
      mod: '${options.modId}',
      key: 'hud',
      value: { ...state },
    });`
    : `for (const [key, value] of Object.entries(state)) {
      ctx.send({ kind: 'state', mod: '${options.modId}', key, value });
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
  opacity: 0.9,
};
const schema = ${JSON.stringify(settingsSchema(options))};

export default defineMock({
  locales: {
    en: {},
    de: {
      'views.${options.view}.title': 'HUD-Beispiel',
      'views.${options.view}.eventHint': 'Backend-Ereignisse erscheinen hier',
    },
  },
  ${native ? '' : 'state,'}
});

export function install(ctx: MockContext) {
  const publishState = () => {
    ${publishBody}
  };
  const changeSetting = (key: keyof typeof settings, value: string | number | boolean) => {
    (settings[key] as string | number | boolean) = value;
    ctx.send({
      kind: 'event',
      name: 'settings.changed',
      payload: { mod: '${options.modId}', key, value },
    });
  };
  const publishSettings = () => ctx.send({
    kind: 'state', mod: 'osfui', key: 'settings',
    value: {
      mods: [{
        id: '${options.modId}', title: schema.title, schema,
        values: { ...settings }, targetVersion: schema.targetVersion,
      }],
    },
  });

  ctx.registerTools([
    { id: 'hud-value', kind: 'button', label: 'Change telemetry' },
    { id: 'hud-alert', kind: 'toggle', label: 'Alert', value: false },
    { id: 'hud-event', kind: 'button', label: 'Push event' },
    { id: 'hud-enabled', kind: 'toggle', label: 'HUD enabled', value: true },
    {
      id: 'hud-anchor', kind: 'cycle', label: 'Anchor', value: 'top-right',
      options: ['top-left', 'top-right', 'bottom-left', 'bottom-right'],
    },
    {
      id: 'hud-opacity', kind: 'cycle', label: 'Opacity', value: '0.9',
      options: ['0.4', '0.7', '0.9', '1'],
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
    } else if (id === 'hud-event') {
      ctx.send(${native
        ? `{ kind: 'event', name: '${options.modId}.notice', payload: { message: 'One-shot native HUD event' } }`
        : `{ kind: 'event', name: '${options.modId}.notice', payload: { args: ['One-shot Papyrus HUD event'] } }`});
    } else if (id === 'hud-enabled') {
      changeSetting('hudEnabled', toolValue === true);
    } else if (id === 'hud-anchor' && typeof toolValue === 'string') {
      changeSetting('anchor', toolValue);
    } else if (id === 'hud-opacity' && typeof toolValue === 'string') {
      changeSetting('opacity', Number(toolValue));
    } else if (id === 'hud-hotkey') {
      ctx.send({
        kind: 'event',
        name: 'ui.hotkey',
        payload: { mod: '${options.modId}', key: 'toggleHud' },
      });
    }
  });

  // install() runs before the view module; defer the native-style initial push
  // so its state subscription is in place. Papyrus state is also replayed by
  // defineMock, and this explicit publish mirrors a save-load republish.
  setTimeout(() => {
    publishState();
    publishSettings();
  }, 0);
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
const schema = ${JSON.stringify(settingsSchema(options))};
const defaults = ${JSON.stringify(settingsDefaults(options))};
const settingValues: Record<string, unknown> = { ...defaults };

export default defineMock({
  locales: {
    en: {},
    de: {
      'views.${options.view}.heading': 'OSF-UI-Starter',
      'views.${options.view}.subtitle': 'Zustände, Ereignisse, Aktionen und Anfragen.',
      'views.${options.view}.connected': 'Verbunden mit OSF UI {version}',
    },
  },
});

export function install(ctx: MockContext) {
  const pushState = () => ctx.send({
    kind: 'state',
    mod: '${options.modId}',
    key: 'state',
    value: { ...state, features: [...state.features] },
  });
  const notice = (message: string) => ctx.send({
    kind: 'event', name: '${options.modId}.notice', payload: { message },
  });
  const publishSettings = () => ctx.send({
    kind: 'state', mod: 'osfui', key: 'settings',
    value: {
      mods: [{
        id: '${options.modId}', title: schema.title, schema,
        values: { ...settingValues }, targetVersion: schema.targetVersion,
      }],
    },
  });
  const changeSetting = (key: string, value: unknown) => {
    settingValues[key] = value;
    ctx.send({
      kind: 'event', name: 'settings.changed',
      payload: { mod: '${options.modId}', key, value },
    });
  };

  ctx.onCommand((kind, name, payload, io) => {
    if (kind === 'request' && name === '${options.modId}.getState') {
      io.resolve({ ...state });
      return true;
    }
    if (kind === 'send' && name === '${options.modId}.increment') {
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
    if (kind === 'request' && name === '${options.modId}.greet') {
      const who = typeof payload.name === 'string' ? payload.name : '';
      if (!who) {
        io.reject('invalid-payload', 'name is required');
        return true;
      }
      const excited = payload.excited === true;
      io.resolve({
        message: state.greeting + ', ' + who + (excited ? '!!' : '!'),
        receivedFromJs: { name: who, excited },
        nativeCount: state.count,
      });
      return true;
    }
  });

  ctx.registerTools([
    { id: 'native-enabled', kind: 'toggle', label: 'Native enabled', value: true },
    { id: 'native-event', kind: 'button', label: 'Push event' },
    { id: 'native-hotkey', kind: 'button', label: 'Fire hotkey callback' },
  ], (id, value) => {
    if (id === 'native-enabled') {
      // Stands in for the player flipping the row in the Mods surface: the
      // settings.changed event and the plugin's own state both follow.
      changeSetting('enabled', value === true);
      state.enabled = value === true;
      state.lastAction = 'Mocked C++ settings callback applied a value';
      pushState();
    } else if (id === 'native-event') {
      notice('One-shot C++ event from the browser mock');
    } else if (id === 'native-hotkey') {
      state.lastAction = 'Mocked C++ hotkey callback fired';
      pushState();
      notice('The native open-view hotkey fired');
      ctx.send({
        kind: 'event', name: 'ui.hotkey',
        payload: { mod: '${options.modId}', key: 'openKey' },
      });
    }
  });

  setTimeout(() => {
    pushState();
    publishSettings();
  }, 0);
}
`;

  return `${mockImport}

// Browser-side mock served to \`osfui dev\`: it stands in for the Papyrus
// script so every round trip works without launching Starfield. Lives at the
// project root so it can never ship with the views.
const state = { greeting: 'Hello from the mocked Papyrus script', clicks: 0 };
const schema = ${JSON.stringify(settingsSchema(options))};
const defaults = ${JSON.stringify(settingsDefaults(options))};
const settingValues: Record<string, unknown> = { ...defaults };

export default defineMock({
  // Mirrors the script's opening OSFUI.SetView* publish; the harness replays
  // these as data.state on every reload, exactly like the real cache.
  state,
  locales: {
    en: {},
    de: {
      'views.${options.view}.heading': 'OSF-UI-Starter',
      'views.${options.view}.subtitle': 'Zustände, Ereignisse, Aktionen und Anfragen.',
      'views.${options.view}.connected': 'Verbunden mit OSF UI {version}',
    },
  },
});

export function install(ctx: MockContext) {
  const publish = () => ctx.send({
    kind: 'state',
    mod: '${options.modId}',
    key: 'clicks',
    value: state.clicks,
  });
  const publishGreeting = () => ctx.send({
    kind: 'state', mod: '${options.modId}', key: 'greeting', value: state.greeting,
  });
  const notice = (text: string) => ctx.send({
    kind: 'event', name: '${options.modId}.notice', payload: { args: [text] },
  });
  const publishEnabled = () => ctx.send({
    kind: 'state', mod: '${options.modId}', key: 'enabled', value: settingValues.enabled,
  });
  const publishSettings = () => ctx.send({
    kind: 'state', mod: 'osfui', key: 'settings',
    value: {
      mods: [{
        id: '${options.modId}', title: schema.title, schema,
        values: { ...settingValues }, targetVersion: schema.targetVersion,
      }],
    },
  });
  const changeSetting = (key: string, value: unknown) => {
    settingValues[key] = value;
    ctx.send({
      kind: 'event', name: 'settings.changed',
      payload: { mod: '${options.modId}', key, value },
    });
    if (key === 'enabled') publishEnabled();
  };

  ctx.onCommand((kind, name, payload, io) => {
    // JavaScript calls a named GLOBAL function on the loose PEX.
    if (name === 'papyrus.call' && payload.script === '${pascalIdentifier(options.modId)}OSFUI') {
      const args = Array.isArray(payload.args) ? payload.args : [];
      // Each branch mirrors the matching function in the .psc — same guard,
      // same published keys, same message. When they drift, the harness proves
      // something the game will not do.
      if (payload.function === 'Refresh') {
        // The script republishes the SETTING value and resets the counter.
        state.greeting = String(settingValues.greeting ?? state.greeting);
        state.clicks = 0;
        publishGreeting();
        publish();
        publishEnabled();
      } else if (payload.function === 'Bump') {
        if (!settingValues.enabled) {
          notice('Backend actions are disabled in Mod Settings');
          return true;
        }
        // Assigns the view's total, exactly as SetViewInt does.
        state.clicks = Number(args[0]) || 0;
        publish();
        notice('JavaScript called a GLOBAL Papyrus function');
      } else if (payload.function === 'OpenSettings') {
        ctx.notify('Papyrus would call OSFUI.OpenMenu()');
      } else if (payload.function === 'Greet') {
        state.greeting = String(settingValues.greeting ?? state.greeting) + ', ' + String(args[0] ?? '');
        publishGreeting();
      }
      return true;
    }
  });

  ctx.registerTools([
    { id: 'papyrus-enabled', kind: 'toggle', label: 'Backend enabled', value: true },
    { id: 'papyrus-event', kind: 'button', label: 'Push event' },
    { id: 'papyrus-hotkey', kind: 'button', label: 'Fire hotkey' },
  ], (id, value) => {
    if (id === 'papyrus-enabled') {
      // Stands in for the player flipping the row in the Mods surface.
      changeSetting('enabled', value === true);
    } else if (id === 'papyrus-event') {
      ctx.send({
        kind: 'event', name: '${options.modId}.notice',
        payload: { args: ['One-shot Papyrus event from the browser mock'] },
      });
    } else if (id === 'papyrus-hotkey') {
      ctx.send({
        kind: 'event', name: 'ui.hotkey',
        payload: { mod: '${options.modId}', key: 'openKey' },
      });
    }
  });

  setTimeout(() => {
    publish();
    publishEnabled();
    publishSettings();
  }, 0);
}
`;
}

function appSource(options) {
  if (options.surface === 'hud') return hudAppSource(options);
  if (options.integration === 'native') return nativeAppSource(options);

  return `import '/shared/osfui.css';
import '/shared/osfui.js';
import './style.css';
import type { OSFUIHelper, PlatformEvents, SettingValue, SettingsData } from '@osfui/cli/view';

const appNode = document.querySelector<HTMLElement>('#app');
if (!appNode) throw new Error('Missing #app element');
const app: HTMLElement = appNode;
// The osf-* classes come from /shared/osfui.css and carry the hover, press,
// focus, and disabled states — plain <button>/<input> get none of them.
app.innerHTML = '<main class="card osf-card"><p class="osf-eyebrow">${options.surface.toUpperCase()} · PAPYRUS BRIDGE</p>' +
  '<h1 data-i18n="views.${options.view}.heading">OSF UI starter</h1>' +
  '<p data-i18n="views.${options.view}.subtitle">Retained state, one-shot events, and direct GLOBAL Papyrus calls.</p>' +
  '<p class="runtime" id="runtime">Bridge setup…</p>' +
  '<section class="state"><span class="osf-eyebrow">Clicks</span><strong id="clicks">—</strong>' +
  '<small id="greeting">Waiting for Papyrus state…</small></section>' +
  '<div class="actions"><button class="osf-btn osf-btn--osf-accent" id="bump">Call Papyrus function</button>' +
  '<button class="osf-btn" id="settings">Open Mod Settings</button>' +
  '<button class="osf-btn" id="close">Close view</button></div>' +
  '<form id="greet"><input class="osf-input" id="name" value="Explorer" aria-label="Name">' +
  '<button class="osf-btn" type="submit">Call with string argument</button></form>' +
  '<small id="setting">Waiting for settings state…</small>' +
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
const runtime = requiredElement('#runtime', HTMLElement);
const bump = requiredElement('#bump', HTMLButtonElement);
const settings = requiredElement('#settings', HTMLButtonElement);
const form = requiredElement('#greet', HTMLFormElement);
const name = requiredElement('#name', HTMLInputElement);
const setting = requiredElement('#setting', HTMLElement);
const close = requiredElement('#close', HTMLButtonElement);
const status = requiredElement('#status', HTMLOutputElement);
const maybeOsfui = window.osfui as OSFUIHelper | undefined;
if (!maybeOsfui) throw new Error('OSF UI helper is unavailable');
const osfui: OSFUIHelper = maybeOsfui;

// Papyrus SetView* -> cached state, replayed whenever this page (re)loads, so
// subscribing at any time still yields the latest value.
osfui.state.on<number>('${options.modId}/clicks', (value) => {
  clicks.textContent = String(value);
});
osfui.state.on<string>('${options.modId}/greeting', (value) => {
  greeting.textContent = value;
});
osfui.state.on<boolean>('${options.modId}/enabled', (value) => {
  bump.disabled = !value;
});
osfui.on<{ args: string[] }>('${options.modId}.notice', (payload) => {
  status.textContent = payload.args[0] || 'Papyrus event received';
});
osfui.papyrus.call('${pascalIdentifier(options.modId)}OSFUI', 'Refresh');

osfui.ready.then(async (info) => {
  runtime.textContent = info.plugin + ' ' + info.version + ' · bridge ' + info.bridgeVersion +
    ' · ' + info.view + ' · locale ' + osfui.i18n.locale;
  await osfui.i18n.ready;
  osfui.i18n.localize(app);
  status.textContent = osfui.i18n.t(
    'views.${options.view}.connected', 'Connected to OSF UI {version}', { version: info.version },
  ) + '; osfui.available = ' + osfui.available;
}).catch((error) => { status.textContent = describe(error); });

// JS -> arbitrary GLOBAL Papyrus functions. Calls are fire-and-forget; the
// functions publish state or events when the view needs to observe an outcome.
let clickTotal = 0;
osfui.state.on<number>('${options.modId}/clicks', (value) => {
  // Keep the local total in step with whatever the backend published, so a
  // reload or a Refresh does not restart the count from zero.
  clickTotal = Number(value) || 0;
});
bump.addEventListener('click', () => {
  // The view carries the total: a recordless GLOBAL Papyrus script has nowhere
  // to accumulate one.
  if (!osfui.papyrus.call('${pascalIdentifier(options.modId)}OSFUI', 'Bump', clickTotal + 1)) {
    status.textContent = 'OSF UI bridge is unavailable';
  }
});
settings.addEventListener('click', () => {
  osfui.papyrus.call('${pascalIdentifier(options.modId)}OSFUI', 'OpenSettings');
});

form.addEventListener('submit', (event) => {
  event.preventDefault();
  osfui.papyrus.call('${pascalIdentifier(options.modId)}OSFUI', 'Greet', name.value);
});

// The settings registry is STATE, so this handler runs immediately with the
// current values and again on every change — no read to issue, no race.
function applySetting(key: string, value: SettingValue) {
  if (key === 'accent' && typeof value === 'string') osfui.theme.applyAccent(app, value);
  setting.textContent = key + ' = ' + JSON.stringify(value);
}
osfui.state.on<SettingsData>('osfui/settings', (registry) => {
  const own = registry.mods.find((mod) => mod.id === '${options.modId}');
  if (!own) return;
  for (const [key, value] of Object.entries(own.values)) applySetting(key, value);
});
// An individual commit is an EVENT: it says what just changed, so it is never
// replayed (the state key above already carries the current value).
osfui.on<PlatformEvents['settings.changed']>('settings.changed', (payload) => {
  if (payload.mod === '${options.modId}') applySetting(payload.key, payload.value);
});
osfui.on<PlatformEvents['ui.hotkey']>('ui.hotkey', (payload) => {
  if (payload.mod === '${options.modId}') status.textContent = 'Hotkey fired: ' + payload.key;
});

// Own Esc/gamepad-B while active, then close explicitly. Drop these two lines
// if your view wants the default native back behavior.
osfui.send('osfui.handleBack', { handle: true });
window.addEventListener('keydown', (event) => {
  if (event.key === 'Escape') osfui.send('close');
});
close.addEventListener('click', () => osfui.send('close'));
osfui.markReady();
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
  --hud-opacity: .9;
  position: fixed;
  inset: 0;
  display: flex;
  padding: 32px;
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
}
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
.hud-panel > small { display: block; margin-top: 10px; color: #a9bdc8; }
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
body { min-height: 100vh; padding: 24px; }
.card { width: min(760px, 94vw); margin: 0 auto; padding: 32px; }
.card h1 { margin: 6px 0 8px; font-size: var(--osf-text-3xl); }
.card > p { margin: 0; color: var(--osf-text-muted); }
.runtime { margin-top: 8px !important; font-family: var(--osf-font-mono); font-size: var(--osf-text-xs); }
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
.card > small {
  display: block;
  margin-top: 18px;
  color: var(--osf-text-muted);
}
output {
  display: block;
  min-height: 24px;
  margin-top: 10px;
  font-family: var(--osf-font-mono);
  font-size: var(--osf-text-xs);
  color: var(--osf-text-muted);
  overflow-wrap: anywhere;
}
`;
}

async function copyPapyrusApi(root) {
  const papyrusRoot = resolve(root, 'tools/papyrus');
  await mkdir(papyrusRoot, { recursive: true });
  await cp(
    resolve(HERE, '..', 'templates/papyrus/OSFUI.psc'),
    resolve(papyrusRoot, 'OSFUI.psc'),
  );
}

// The settings surface ships no view, so it needs none of the npm toolchain:
// the schema is a drop-in JSON file and the script compiles with the Creation
// Kit alone.
async function scaffoldSettings(root, options) {
  for (const file of settingsOnlyFiles(options)) {
    await put(root, file.path, file.content);
  }
  await copyPapyrusApi(root);
  await put(root, 'README.md', settingsOnlyReadme(options));
  return root;
}

async function scaffold(options) {
  const root = resolve(options.directory);
  await mkdir(root, { recursive: true });
  if ((await readdir(root)).length) throw new Error(`Directory is not empty: ${root}`);
  if (options.surface === 'settings') return scaffoldSettings(root, options);
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
      ? ['mod/Scripts/**/*.pex']
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
    await copyPapyrusApi(root);
  }
  await put(root, `osfui.config.ts`, `import { defineConfig } from '@osfui/cli';

export default defineConfig({
  modId: '${options.modId}',
${backendConfig(options)}  views: [{
    id: '${options.view}',
    title: '${options.view.replaceAll('-', ' ')}',
    description: 'Generated ${options.surface} starter for ${options.modId}',
    kind: '${options.surface}',
    width: ${options.surface === 'hud' ? 1920 : 1200},
    height: ${options.surface === 'hud' ? 1080 : 720},
    accent: '#7bdcff',
    targetVersion: '2.0.0',
${options.surface === 'hud' ? `    openOnStart: true,
` : `    pausesGame: false,
    readySignal: true,
`}  }],
});
`);
  await put(root, `osfui.mock.ts`, mockSource(options));
  await put(root, `${viewRoot}/index.html`, `<!doctype html>
<html lang="en"><head><meta charset="UTF-8"><meta name="viewport" content="width=device-width,initial-scale=1">
<title>${options.view}</title></head><body><div id="app"></div><script type="module" src="./main.ts"></script></body></html>
`);
  await put(root, `${viewRoot}/main.ts`, appSource(options));
  await put(root, `${viewRoot}/style.css`, styleSource(options));
  const generalReadme = options.surface === 'menu' && options.integration === 'papyrus'
    ? ''
    : `Run \`npm run dev\` for instant browser HMR. Run \`npm run dev:game -- --deploy "path-to-MO2-mods"\`
to create this mod's folder under MO2 and sync into Starfield with temporary
author mode, automatic view reload, and F12 DevTools.

Use \`npm run package\` to create a release-ready zip. Files under \`mod/\`
are copied into the mod archive beside the generated view.

`;
  await put(root, 'README.md', `# ${packageJson.name}

A runnable OSF UI ${options.surface} starter: one worked example of each way a
view and its backend talk to each other, plus a small settings schema. It is
deliberately not a catalogue — the documentation linked below covers the rest.

${generalReadme}${backendGuide(options)}
${docsGuide(options)}`);
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
    console.log('npm create osfui@latest [directory] ' +
      '[-- --mod-id author.mod --view main --surface menu|hud|settings --integration papyrus|native]');
    return;
  }
  options.directory = options._[0];
  const interactive = await promptMissing(options);
  validate(options);
  const root = await scaffold(options);
  // The settings surface has no package.json to install into.
  if (!options.noInstall && options.surface !== 'settings') await install(root);
  const firstCommands = options.surface === 'settings'
    ? './build-deploy.ps1 -Mo2Mods "path-to-MO2-mods"'
    : options.integration === 'papyrus'
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
