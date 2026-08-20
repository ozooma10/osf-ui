import { defineMock, type MockContext } from '@osfui/cli';

// Browser-side mock served to `osfui dev`: it stands in for the Papyrus
// script so every round trip works without launching Starfield. Lives at the
// project root so it can never ship with the views.
const state = { greeting: 'Hello from the mocked Papyrus script', clicks: 0 };
const schema = {"$schema":"https://github.com/ozooma10/osf-ui/blob/main/docs/schema/settings-schema.schema.json","id":"__OSFUI_MOD_ID_SQ__","title":"__OSFUI_DISPLAY_NAME__","description":"Settings for __OSFUI_VIEW_TITLE__.","version":1,"targetVersion":"__OSFUI_RELEASE_VERSION__","accent":"#7bdcff","groups":[{"id":"general","label":"General","settings":[{"key":"enabled","label":"Enable mod-backend actions","type":"bool","default":true},{"key":"mode","label":"Display mode","type":"enum","options":["compact","detailed"],"optionLabels":["Compact","Detailed"],"default":"detailed"},{"key":"intensity","label":"Example integer","type":"int","min":0,"max":100,"step":5,"default":65,"enabledWhen":{"key":"enabled","eq":true}},{"key":"greeting","label":"Mod-backend greeting","type":"string","default":"Hello from OSF UI","maxLength":80},{"key":"accent","label":"Accent colour","type":"string","widget":"color","default":"#7bdcff"},{"key":"openKey","label":"Open example view","type":"key","default":"F9","allowUnbound":true}]}]};
const defaults = {"enabled":true,"mode":"detailed","intensity":65,"greeting":"Hello from OSF UI","accent":"#7bdcff","openKey":"F9"};
const settingValues: Record<string, unknown> = { ...defaults };

export default defineMock({
  // Mirrors the script's opening OSFUI_View.SetState publish; the harness
  // replays these retained state values on every reload, like the real cache.
  state,
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
  const publish = () => ctx.send({
    kind: 'state',
    mod: '__OSFUI_MOD_ID_SQ__',
    key: 'clicks',
    value: state.clicks,
  });
  const publishGreeting = () => ctx.send({
    kind: 'state', mod: '__OSFUI_MOD_ID_SQ__', key: 'greeting', value: state.greeting,
  });
  const notice = (text: string) => ctx.send({
    kind: 'event', name: '__OSFUI_MOD_ID_SQ__.notice', payload: { args: [text] },
  });
  const publishEnabled = () => ctx.send({
    kind: 'state', mod: '__OSFUI_MOD_ID_SQ__', key: 'enabled', value: settingValues.enabled,
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
    if (key === 'enabled') publishEnabled();
  };

  const handleEndpoint: Parameters<NonNullable<MockContext['onEndpoint']>>[0] = (kind, name, payload, io) => {
    // JavaScript calls a named GLOBAL function on the loose PEX.
    if (name === 'papyrus.call' && payload.script === '__OSFUI_SCRIPT_NAME__') {
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
          notice('Mod-backend actions are disabled in Mod Settings');
          return true;
        }
        // Assigns the view's total, exactly as OSFUI_View.SetState does.
        state.clicks = Number(args[0]) || 0;
        publish();
        notice('JavaScript called a GLOBAL Papyrus function');
      }
      return true;
    }
  };
  ctx.onEndpoint(handleEndpoint);

  ctx.registerTools([
    { id: 'papyrus-enabled', kind: 'toggle', label: 'Mod backend enabled', value: true },
    { id: 'papyrus-event', kind: 'button', label: 'Push event' },
    { id: 'papyrus-hotkey', kind: 'button', label: 'Fire hotkey' },
  ], (id, value) => {
    if (id === 'papyrus-enabled') {
      // Stands in for the player flipping the row in Mod Settings.
      changeSetting('enabled', value === true);
    } else if (id === 'papyrus-event') {
      ctx.send({
        kind: 'event', name: '__OSFUI_MOD_ID_SQ__.notice',
        payload: { args: ['One-shot Papyrus event from the browser mock'] },
      });
    } else if (id === 'papyrus-hotkey') {
      ctx.send({
        kind: 'event', name: 'ui.hotkey',
        payload: { mod: '__OSFUI_MOD_ID_SQ__', key: 'openKey' },
      });
    }
  });

  setTimeout(() => {
    publish();
    publishEnabled();
    publishSettings();
  }, 0);
}
