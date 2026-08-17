// The built-in views' dev mock for `osfui dev`: a simulated OSF UI runtime and
// bridge with fixture mod-backend behavior (devmock/mockbridge — settings
// round-trips, schemas, key capture, health scenarios, locales, drag-drop),
// installed as an @osfui/cli mock module.
//
// install() runs inside the view iframe before the view's module entry.
// mockbridge takes over window.osfui.postMessage wholesale; the harness
// detects that and drains any queued bridge messages into it. The old harness
// toolbar's controls become registered shell tools here.

import type { MockContext } from '@osfui/cli';

import { installMock, type MockApi } from './devmock/mockbridge';

const HEALTH = ['clean', 'warnings', 'errors', 'mixed', 'resolved', 'catalog'];

export function install(ctx: MockContext): void {
  const mock: MockApi = installMock({ selfView: ctx.meta.qualifiedId });

  // The shell's locale control targets the CLI's simple-tier runtime, which
  // a takeover mock bypasses — route it into the mock's own applyLocale
  // (catalog fallback chains, pseudo wrap, full re-push).
  window.addEventListener('message', (event) => {
    if (event.origin !== location.origin) return;
    const data = event.data as { source?: string; kind?: string; action?: string; locale?: string };
    if (!data || data.source !== 'osfui-harness' || data.kind !== 'control' || data.action !== 'locale') return;
    void mock.locale(String(data.locale || 'en'));
  });

  ctx.registerTools(
    [
      { id: 'reset', kind: 'button', label: 'Reset values', title: 'Clear stored setting values' },
      {
        id: 'fixtures',
        kind: 'toggle',
        label: 'Sample views',
        value: mock.fixturesOn(),
        title:
          'Show fictional sample panels/HUDs that exercise every catalog state (failed load, HUD live/hidden, …)',
      },
      {
        id: 'health',
        kind: 'cycle',
        label: 'Health',
        options: HEALTH,
        value: mock.healthScenario(),
        title: 'Cycle the local System Health scenario',
      },
      {
        id: 'hotkey',
        kind: 'button',
        label: 'Hotkey',
        title: 'Inject a ui.hotkey message for the first type:"key" setting in the registry',
      },
      { id: 'pad-lb', kind: 'button', label: 'LB', title: 'Inject a ui.gamepad LB down-edge (cycles the rail)' },
      { id: 'pad-rb', kind: 'button', label: 'RB', title: 'Inject a ui.gamepad RB down-edge (cycles the rail)' },
    ],
    (id, value) => {
      if (id === 'reset') mock.reset();
      else if (id === 'fixtures') mock.fixtures(value === true);
      else if (id === 'health') mock.health(String(value));
      else if (id === 'hotkey') mock.hotkey();
      else if (id === 'pad-lb') mock.gamepad('LB');
      else if (id === 'pad-rb') mock.gamepad('RB');
    },
  );
}
