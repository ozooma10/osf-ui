// @vitest-environment jsdom
//
// The handoff view against the *real* frozen helper, not a fake bridge.
//
// This surface is the one the runtime shows while a target view's renderer is
// still starting, so the thing worth pinning is the whole chain: a native
// `handoff.state` frame -> shared-kit dispatch -> Preact render -> outbound
// `ui.command` envelopes. A mocked bridge would pass even if the view stopped
// speaking the shipped helper's protocol.

import { afterEach, beforeEach, describe, expect, it } from 'vitest';
import { readFileSync } from 'node:fs';
import { resolve } from 'node:path';
import { render } from 'preact';
import { act } from 'preact/test-utils';
import { windowBridge } from '@lib/bridge';
import { App } from '@views/osfui/handoff/App';

const HELPER = readFileSync(resolve(process.cwd(), 'src/shared-kit/osfui.js'), 'utf8');

interface Frame {
  type: string;
  payload: Record<string, unknown>;
}

let host: HTMLElement;

// `receive` takes a loose record, not HandoffStatePayload: these are wire
// frames, and one case deliberately pushes an off-contract `phase`.
function mount(): { frames: Frame[]; receive(payload: Record<string, unknown>): void } {
  const frames: Frame[] = [];
  // The helper decorates whatever `window.osfui` it finds; a bare object with
  // postMessage is exactly what the native runtime injects before it runs.
  (window as unknown as { osfui: unknown }).osfui = {
    postMessage(json: string) {
      frames.push(JSON.parse(json) as Frame);
    },
  };
  new Function(HELPER)();

  host = document.createElement('div');
  document.body.appendChild(host);
  act(() => {
    render(<App bridge={windowBridge} />, host);
  });

  return {
    frames,
    receive(payload) {
      act(() => {
        window.osfui!.onMessage!(JSON.stringify({ type: 'handoff.state', payload }));
      });
    },
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

    expect(app.frames.map((frame) => frame.payload['command'])).toEqual([
      'osfui.handoffRetry',
      'close',
    ]);
  });

  it('closes on Escape from anywhere on the page', () => {
    const app = mount();
    window.dispatchEvent(new KeyboardEvent('keydown', { key: 'Escape' }));

    expect(app.frames.map((frame) => frame.payload['command'])).toEqual(['close']);
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
