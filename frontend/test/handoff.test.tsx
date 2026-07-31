// @vitest-environment jsdom
//
// The handoff view against the *real* frozen helper, not a fake bridge.
//
// This surface is the one the runtime shows while a target view's renderer is
// still starting, so the thing worth pinning is the whole chain: a native
// `state` frame for `osfui/handoff` -> shared-kit dispatch -> Preact render ->
// outbound `send` envelopes. A mocked bridge would pass even if the view
// stopped speaking the shipped helper's protocol.
//
// Protocol 2.0 moved this surface from a `handoff.state` PUSH to a state KEY,
// which is the difference between "you had to be listening" and "it is replayed
// to every document" — see the replay case below.

import { afterEach, beforeEach, describe, expect, it } from 'vitest';
import { readFileSync } from 'node:fs';
import { resolve } from 'node:path';
import { render } from 'preact';
import { act } from 'preact/test-utils';
import { windowBridge } from '@lib/bridge';
import { App } from '@views/osfui/handoff/App';

const HELPER = readFileSync(resolve(process.cwd(), 'src/shared-kit/osfui.js'), 'utf8');

/** One web -> native envelope, as the helper posts it. */
interface Frame {
  kind: string;
  name: string;
  payload: Record<string, unknown>;
  id?: string;
}

let host: HTMLElement;

/**
 * Load the shipped helper over a bare injected bridge — a `postMessage` slot is
 * exactly what the native runtime provides before any script of ours runs.
 */
function installHelper(): Frame[] {
  const frames: Frame[] = [];
  (window as unknown as { osfui: unknown }).osfui = {
    postMessage(json: string) {
      frames.push(JSON.parse(json) as Frame);
    },
  };
  new Function(HELPER)();
  return frames;
}

/**
 * Deliver `osfui/handoff`. `value` is a loose record, not HandoffState: these
 * are wire frames, and one case deliberately pushes an off-contract `phase`.
 */
function publish(value: Record<string, unknown>): void {
  act(() => {
    window.osfui!.onMessage!(
      JSON.stringify({ kind: 'state', mod: 'osfui', key: 'handoff', value }),
    );
  });
}

function renderApp(): void {
  host = document.createElement('div');
  document.body.appendChild(host);
  act(() => {
    render(<App bridge={windowBridge} />, host);
  });
}

function mount(): {
  frames: Frame[];
  /** Every outbound endpoint name in order, the boot greeting included. */
  names(): string[];
  receive(value: Record<string, unknown>): void;
} {
  const frames = installHelper();
  renderApp();
  return {
    frames,
    names: () => frames.map((frame) => frame.name),
    receive: publish,
  };
}

beforeEach(() => {
  document.documentElement.removeAttribute('style');
  document.body.removeAttribute('data-live');
  document.body.removeAttribute('data-phase');
});

afterEach(() => {
  if (host) render(null, host);
  document.body.innerHTML = '';
});

describe('first-load handoff surface', () => {
  it('greets the host itself — the page-initiated handshake is the only boot path', () => {
    const app = mount();
    // Sent by the helper the moment it loads, before the view renders a thing.
    // Nothing waits on a greeting that might already have been missed.
    expect(app.frames[0]).toEqual({ kind: 'send', name: 'osfui.hello', payload: {} });
  });

  it('renders the cold chrome before any state arrives', () => {
    mount();

    // data-live is what style.css keys the "connected" look on: an unpushed
    // surface must stay dark rather than claim a link it has not got.
    expect(document.body.dataset['live']).toBeUndefined();
    expect(document.querySelector('#title')?.textContent).toBe('INTERFACE');
    expect(document.querySelector('#owner')?.textContent).toBe('LOCAL SYSTEM');
    expect(document.querySelector('#target')?.textContent).toBe('UNRESOLVED');
    expect(document.querySelector('#channel')?.textContent).toBe('OSF-LINK 00');
    expect((document.querySelector('#actions') as HTMLElement).hidden).toBe(true);
  });

  it('renders the target identity and inherited accent for a linking state', () => {
    const app = mount();

    app.receive({
      target: 'demo.mod/terminal',
      mod: 'demo.mod',
      title: 'Cargo terminal',
      accent: '#e6904a',
      phase: 'linking',
      retry: false,
    });

    expect(document.body.dataset['live']).toBe('true');
    expect(document.body.dataset['phase']).toBe('linking');
    expect(document.querySelector('#title')?.textContent).toBe('CARGO TERMINAL');
    expect(document.querySelector('#owner')?.textContent).toBe('DEMO MOD');
    expect(document.querySelector('#target')?.textContent).toBe('DEMO.MOD/TERMINAL');
    expect((document.querySelector('#actions') as HTMLElement).hidden).toBe(true);
    expect(document.documentElement.style.getPropertyValue('--osf-accent')).toBe('#e6904a');
  });

  it('comes back live after a reload, because the state is REPLAYED', () => {
    // The document is destroyed and rebuilt mid-handoff (F5, a renderer
    // restart). The host replays `osfui/handoff` before the view mounts, so
    // subscribing IS the read and the very first paint is already connected —
    // where a 1.x one-shot push would have left the cold chrome up forever.
    installHelper();
    publish({
      target: 'demo.mod/terminal',
      mod: 'demo.mod',
      title: 'Cargo terminal',
      accent: '',
      phase: 'retrying',
      retry: false,
    });
    renderApp();

    expect(document.body.dataset['live']).toBe('true');
    expect(document.body.dataset['phase']).toBe('retrying');
    expect(document.querySelector('#title')?.textContent).toBe('CARGO TERMINAL');
    expect(document.querySelector('#status')?.textContent).toBe('SIGNAL INTERRUPTED // REACQUIRING');
  });

  it('offers working retry and cancel controls after a failed link', () => {
    const app = mount();
    app.receive({
      target: 'demo.mod/terminal',
      mod: 'demo.mod',
      title: 'Cargo terminal',
      accent: '',
      phase: 'error',
      retry: true,
    });

    expect((document.querySelector('#actions') as HTMLElement).hidden).toBe(false);
    expect(document.querySelector('#status')?.textContent).toContain('LINK FAILED');
    // Retry takes focus so a controller lands on it without any spatial nav —
    // this view deliberately does not ship padnav.js.
    expect(document.activeElement).toBe(document.querySelector('#retry'));

    (document.querySelector('#retry') as HTMLButtonElement).click();
    (document.querySelector('#close') as HTMLButtonElement).click();

    // Both are SENDs: neither has an outcome to await, so neither carries an id.
    expect(app.names()).toEqual(['osfui.hello', 'osfui.handoffRetry', 'close']);
    for (const frame of app.frames) {
      expect(frame.kind).toBe('send');
      expect(frame.id).toBeUndefined();
    }
  });

  it('closes on Escape from anywhere on the page', () => {
    const app = mount();
    window.dispatchEvent(new KeyboardEvent('keydown', { key: 'Escape' }));

    expect(app.names()).toEqual(['osfui.hello', 'close']);
  });

  it('falls back to the linking copy for an unknown phase', () => {
    const app = mount();
    // `phase` is untrusted JSON off the wire; an unrecognised value must not
    // blank the panel by indexing the copy table to undefined.
    app.receive({ target: 'a/b', mod: 'a', title: 'B', accent: '', phase: 'bogus', retry: false });

    expect(document.body.dataset['phase']).toBe('linking');
    expect(document.querySelector('#status')?.textContent).toBe('ESTABLISHING LOCAL LINK');
  });
});
