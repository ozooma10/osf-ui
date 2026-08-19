import { defineMock, type MockContext } from '@osfui/cli';

// The browser harness mirrors native/src/main.cpp so every round trip works
// without launching Starfield. This file stays at project root and never ships.
const state = {
  count: 0,
  enabled: true,
  greeting: 'Hello from the mocked C++ plugin',
  lastAction: 'Browser mock initialized',
  features: ['typed JSON', 'sends', 'requests', 'native pushes', 'settings', 'hotkeys'],
};
const schema = {"$schema":"https://github.com/ozooma10/osf-ui/blob/main/docs/schema/settings-schema.schema.json","id":"__OSFUI_MOD_ID_SQ__","title":"__OSFUI_DISPLAY_NAME__","description":"Settings for __OSFUI_VIEW_TITLE__.","version":1,"targetVersion":"__OSFUI_RELEASE_VERSION__","accent":"#7bdcff","groups":[{"id":"general","label":"General","settings":[{"key":"enabled","label":"Enable mod-backend actions","type":"bool","default":true},{"key":"mode","label":"Display mode","type":"enum","options":["compact","detailed"],"optionLabels":["Compact","Detailed"],"default":"detailed"},{"key":"intensity","label":"Example integer","type":"int","min":0,"max":100,"step":5,"default":65,"enabledWhen":{"key":"enabled","eq":true}},{"key":"greeting","label":"Mod-backend greeting","type":"string","default":"Hello from OSF UI","maxLength":80},{"key":"accent","label":"Accent colour","type":"string","widget":"color","default":"#7bdcff"},{"key":"openKey","label":"Open example view","type":"key","default":"F9","allowUnbound":true},{"type":"action","key":"recalibrate","label":"Run native action","command":"__OSFUI_MOD_ID_SQ__.recalibrate","style":"accent","confirm":"Run the generated native request example?"}]}]};
const defaults = {"enabled":true,"mode":"detailed","intensity":65,"greeting":"Hello from OSF UI","accent":"#7bdcff","openKey":"F9"};
const settingValues: Record<string, unknown> = { ...defaults };

export default defineMock({
  locales: {
    en: {},
    de: {
      'views.__OSFUI_VIEW_ID__.heading': 'OSF-UI-Starter',
      'views.__OSFUI_VIEW_ID__.subtitle': 'Zustände, Ereignisse, Aktionen und Anfragen.',
      'views.__OSFUI_VIEW_ID__.connected': 'Verbunden mit OSF UI {version}',
    },
  },
});

export function install(ctx: MockContext) {
  const pushState = () => ctx.send({
    kind: 'state',
    mod: '__OSFUI_MOD_ID_SQ__',
    key: 'state',
    value: { ...state, features: [...state.features] },
  });
  const notice = (message: string) => ctx.send({
    kind: 'event', name: '__OSFUI_MOD_ID_SQ__.notice', payload: { message },
  });
  const publishSettings = () => ctx.send({
    kind: 'state', mod: 'osfui', key: 'settings',
    value: {
      mods: [{
        id: '__OSFUI_MOD_ID_SQ__', title: schema.title, schema,
        values: { ...settingValues }, targetVersion: schema.targetVersion,
      }],
      keyboard: { layout: 'en-US', labels: { F8: 'F8', F9: 'F9' } },
    },
  });
  const changeSetting = (key: string, value: unknown) => {
    settingValues[key] = value;
    ctx.send({
      kind: 'event', name: 'settings.changed',
      payload: { mod: '__OSFUI_MOD_ID_SQ__', key, value },
    });
  };

  const handleEndpoint: Parameters<NonNullable<MockContext['onEndpoint']>>[0] = (kind, name, payload, io) => {
    if (kind === 'request' && name === '__OSFUI_MOD_ID_SQ__.getState') {
      io.resolve({ ...state });
      return true;
    }
    if (kind === 'send' && name === '__OSFUI_MOD_ID_SQ__.increment') {
      const requested = Number(payload.amount);
      const amount = Number.isFinite(requested) ? Math.max(-10, Math.min(10, requested)) : 1;
      if (state.enabled) {
        state.count += amount;
        state.lastAction = 'JavaScript called a fire-and-forget send endpoint';
        pushState();
      } else {
        notice('The native counter is disabled in Mod Settings');
      }
      return true;
    }
    if (kind === 'request' && name === '__OSFUI_MOD_ID_SQ__.greet') {
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
  };
  if (ctx.onEndpoint) ctx.onEndpoint(handleEndpoint);
  else ctx.onCommand(handleEndpoint as Parameters<MockContext['onCommand']>[0]);

  ctx.registerTools([
    { id: 'native-enabled', kind: 'toggle', label: 'Native enabled', value: true },
    { id: 'native-event', kind: 'button', label: 'Push event' },
    { id: 'native-hotkey', kind: 'button', label: 'Fire hotkey callback' },
  ], (id, value) => {
    if (id === 'native-enabled') {
      // Stands in for the player flipping the row in Mod Settings: the
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
        payload: { mod: '__OSFUI_MOD_ID_SQ__', key: 'openKey' },
      });
    }
  });

  setTimeout(() => {
    pushState();
    publishSettings();
  }, 0);
}
