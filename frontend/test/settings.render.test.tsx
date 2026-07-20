// @vitest-environment jsdom
//
// Pins the settings widget quirks. Each `it` names the exact behaviour it
// guards; these read as arbitrary but a refactor that "cleans them up" breaks
// a shipped view.

import { describe, it, expect, afterEach } from 'vitest';
import { makeBridge, mount, unmount, flush } from './helpers/settingsHarness';
import { WIDGETS, VIEWS } from './helpers/settingsFixtures';

afterEach(unmount);

/** Mount, deliver the widget mod, then select it in the rail. */
async function mountKit() {
  const bridge = makeBridge();
  const el = await mount(bridge);
  bridge.emit('settings.data', WIDGETS);
  bridge.emit('views.data', VIEWS);
  await flush();
  // Land on Home by default; click the Acme Kit rail entry.
  const railItem = [...el.querySelectorAll<HTMLButtonElement>('.rail-item')].find((b) =>
    b.textContent!.includes('Acme Kit'),
  );
  railItem!.click();
  await flush();
  return { bridge, el };
}

describe('settings widget rendering', () => {
  it('bool renders as a button[role=switch] with state in aria-pressed', async () => {
    const { el } = await mountKit();
    const sw = el.querySelector<HTMLButtonElement>('#ctl-acme\\.kit-boolOn');
    expect(sw).not.toBeNull();
    expect(sw!.tagName).toBe('BUTTON');
    expect(sw!.getAttribute('role')).toBe('switch');
    expect(sw!.getAttribute('aria-pressed')).toBe('true');
  });

  it('bool initial state is value === true STRICTLY (truthy non-true is off)', async () => {
    const { el } = await mountKit();
    const truthy = el.querySelector<HTMLButtonElement>('#ctl-acme\\.kit-boolTruthy');
    // The stored value is `1` — truthy, but not the boolean `true`.
    expect(truthy!.getAttribute('aria-pressed')).toBe('false');
  });

  it('bool click commits the toggled value over the bridge', async () => {
    const { bridge, el } = await mountKit();
    el.querySelector<HTMLButtonElement>('#ctl-acme\\.kit-boolOn')!.click();
    await flush();
    const i = bridge.indexOf('settings.set');
    expect(bridge.requests[i]!.fields).toEqual({ mod: 'acme.kit', key: 'boolOn', value: false });
  });

  it('slider commits on change, NOT on input; input only repaints the readout', async () => {
    const { bridge, el } = await mountKit();
    const slider = el.querySelector<HTMLInputElement>('#ctl-acme\\.kit-slide')!;
    const before = bridge.countRequests('settings.set');

    // input: repaint only.
    slider.value = '75';
    slider.dispatchEvent(new Event('input', { bubbles: true }));
    await flush();
    expect(bridge.countRequests('settings.set')).toBe(before); // no commit yet
    const readout = el.querySelector('#ctl-acme\\.kit-slide')!.closest('.control')!.querySelector('.osf-value');
    expect(readout!.textContent).toContain('75');

    // change: commit.
    slider.value = '80';
    slider.dispatchEvent(new Event('change', { bubbles: true }));
    await flush();
    const i = bridge.indexOf('settings.set', before === 0 ? 0 : before);
    expect(bridge.requests[bridge.requests.length - 1]!.fields).toEqual({
      mod: 'acme.kit',
      key: 'slide',
      value: 80,
    });
  });

  it('stepper repaints .osf-stepper-val, NOT .osf-value', async () => {
    const { bridge, el } = await mountKit();
    const stepper = el.querySelector('#ctl-acme\\.kit-step')!;
    expect(stepper.classList.contains('osf-stepper')).toBe(true);
    expect(stepper.querySelector('.osf-stepper-val')).not.toBeNull();
    expect(stepper.closest('.control')!.querySelector('.osf-value')).toBeNull();

    // + snaps onto the grid from min (3 -> 6) and commits.
    const buttons = stepper.querySelectorAll<HTMLButtonElement>('.osf-stepper-btn');
    buttons[1]!.click(); // the "+"
    await flush();
    expect(bridge.requests[bridge.requests.length - 1]!.fields).toEqual({
      mod: 'acme.kit',
      key: 'step',
      value: 6,
    });
  });

  it('enum is segmented only when widget=segmented AND 1..5 options, else <select>', async () => {
    const { el } = await mountKit();
    // 3 options + widget:segmented -> segmented.
    const seg = el.querySelector('#ctl-acme\\.kit-segMode')!;
    expect(seg.classList.contains('osf-segmented')).toBe(true);
    expect(seg.querySelectorAll('.osf-segment').length).toBe(3);
    // 6 options -> select regardless of anything.
    const pick = el.querySelector('#ctl-acme\\.kit-pickMode')!;
    expect(pick.tagName).toBe('SELECT');
  });

  it('flags recommits the whole array in canonical declared order', async () => {
    const { bridge, el } = await mountKit();
    const group = el.querySelector('#ctl-acme\\.kit-flagSet')!;
    // Declared order is [read, write, exec] and the stored value is
    // [read, write], so checking exec must commit declared order, not
    // insertion order.
    const boxes = group.querySelectorAll<HTMLInputElement>('.osf-flag-box');
    const exec = [...boxes].find((b) => b.value === 'exec')!;
    exec.checked = true;
    exec.dispatchEvent(new Event('change', { bubbles: true }));
    await flush();
    expect(bridge.requests[bridge.requests.length - 1]!.fields).toEqual({
      mod: 'acme.kit',
      key: 'flagSet',
      value: ['read', 'write', 'exec'],
    });
  });

  it('color accepts uppercase hex and reverts to the last committed value on junk', async () => {
    const { bridge, el } = await mountKit();
    const hex = el.querySelector<HTMLInputElement>('#ctl-acme\\.kit-colorHex .osf-color-hex')!;
    hex.value = '#AABBCC';
    hex.dispatchEvent(new Event('change', { bubbles: true }));
    await flush();
    expect(bridge.requests[bridge.requests.length - 1]!.fields).toEqual({
      mod: 'acme.kit',
      key: 'colorHex',
      value: '#AABBCC',
    });
    const commits = bridge.countRequests('settings.set');

    // Junk reverts to the last committed colour (#AABBCC), not the session start.
    hex.value = 'nonsense';
    hex.dispatchEvent(new Event('change', { bubbles: true }));
    await flush();
    expect(bridge.countRequests('settings.set')).toBe(commits); // no new commit
    expect(hex.value).toBe('#AABBCC');
  });

  it('key renders .osf-key-wrap with an unbind ✕ only when allowUnbound AND a value exists', async () => {
    const { el } = await mountKit();
    // bindKey has allowUnbound + value "K" -> wrap + clear.
    const wrap = el.querySelector('#ctl-acme\\.kit-bindKey')!.closest('.osf-key-wrap');
    expect(wrap).not.toBeNull();
    expect(wrap!.querySelector('.osf-key-clear')).not.toBeNull();
    // The framework toggleKey has no allowUnbound -> no wrap (unchecked here;
    // needs the osfui mod selected).
  });

  it('note style is whitelisted and its body runs through micro-markdown', async () => {
    const { el } = await mountKit();
    const notes = el.querySelectorAll('.osf-note');
    const warn = [...notes].find((n) => n.textContent!.includes('bold'))!;
    expect(warn.classList.contains('osf-note--warn')).toBe(true);
    expect(warn.querySelector('strong')!.textContent).toBe('bold');
    // The unknown style "evil" falls back to info.
    const evil = [...notes].find((n) => n.textContent === 'sneaky')!;
    expect(evil.classList.contains('osf-note--info')).toBe(true);
    expect(evil.classList.contains('osf-note--evil')).toBe(false);
  });

  it('action refuses a reserved-namespace command with a danger toast, no send', async () => {
    const { bridge, el } = await mountKit();
    const before = bridge.requests.length;
    const reserved = [...el.querySelectorAll<HTMLButtonElement>('.row--action .osf-btn')].find(
      (b) => b.textContent === 'Reserved',
    )!;
    reserved.click();
    await flush();
    // ui.doThing never left the client.
    expect(bridge.requests.length).toBe(before);
    expect(el.querySelector('.toast--danger')).not.toBeNull();
  });

  it('a valid action fires the namespaced command', async () => {
    const { bridge, el } = await mountKit();
    const go = [...el.querySelectorAll<HTMLButtonElement>('.row--action .osf-btn')].find(
      (b) => b.textContent === 'Run it',
    )!;
    go.click();
    await flush();
    expect(bridge.requests[bridge.requests.length - 1]!.command).toBe('acme.kit.run');
    // In-flight buttons carry `pending` and are disabled.
    expect(go.classList.contains('pending')).toBe(true);
    expect(go.disabled).toBe(true);
  });

  it('row id is mod-prefixed (ctl-<mod>-<key>) and label[for] matches', async () => {
    const { el } = await mountKit();
    const row = el.querySelector('.row[data-key="boolOn"]')!;
    const label = row.querySelector<HTMLLabelElement>('label.row-label')!;
    expect(label.htmlFor).toBe('ctl-acme.kit-boolOn');
    expect(el.querySelector('#ctl-acme\\.kit-boolOn')).not.toBeNull();
  });

  it('a keyless setting is SKIPPED, not rendered', async () => {
    const { el } = await mountKit();
    // The "No key here" bool has no key — it must not appear.
    const rows = [...el.querySelectorAll('.row-label')].map((n) => n.textContent);
    expect(rows).not.toContain('No key here');
  });

  it('control cell order is [reset ↺][optional readout][control]', async () => {
    const { el } = await mountKit();
    const control = el.querySelector('.row[data-key="slide"] .control')!;
    const kids = [...control.children];
    expect(kids[0]!.classList.contains('row-reset')).toBe(true);
    expect(kids[1]!.classList.contains('osf-value')).toBe(true);
    expect(kids[2]!.classList.contains('osf-range')).toBe(true);
  });

  it('an unknown setting type renders read-only .row--unknown with the escaped-quote copy', async () => {
    const { el } = await mountKit();
    const unknown = el.querySelector('.row--unknown')!;
    expect(unknown).not.toBeNull();
    expect(unknown.querySelector('.row-hint')!.textContent).toBe(
      'Type "quantum" needs a newer OSF UI.',
    );
    // It has no data-key, so it never participates in search/conditions.
    expect(unknown.getAttribute('data-key')).toBeNull();
  });

  it('the reset ↺ stays enabled when enabledWhen disables the control', async () => {
    // Uses a fresh mod so the gate is unambiguous.
    const bridge = makeBridge();
    const el = await mount(bridge);
    bridge.emit('settings.data', {
      mods: [
        {
          id: 'g.mod',
          title: 'Gated',
          values: { master: false, child: true },
          schema: {
            groups: [
              {
                settings: [
                  { key: 'master', label: 'Master', type: 'bool', default: false },
                  {
                    key: 'child',
                    label: 'Child',
                    type: 'bool',
                    default: false,
                    enabledWhen: { key: 'master', truthy: true },
                  },
                ],
              },
            ],
          },
        },
      ],
    } as never);
    await flush();
    el.querySelector<HTMLButtonElement>('.rail-item')!;
    [...el.querySelectorAll<HTMLButtonElement>('.rail-item')]
      .find((b) => b.textContent!.includes('Gated'))!
      .click();
    await flush();

    const row = el.querySelector('.row[data-key="child"]')!;
    expect(row.classList.contains('disabled')).toBe(true);
    expect(row.querySelector<HTMLButtonElement>('#ctl-g\\.mod-child')!.disabled).toBe(true);
    expect(row.querySelector<HTMLButtonElement>('.row-reset')!.disabled).toBe(false);
  });
});
