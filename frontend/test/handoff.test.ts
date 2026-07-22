// @vitest-environment jsdom

import { beforeEach, describe, expect, it } from 'vitest';
import { readFileSync } from 'node:fs';
import { resolve } from 'node:path';

const VIEW = resolve(process.cwd(), 'src/views/osfui/handoff');
const INDEX = readFileSync(resolve(VIEW, 'index.html'), 'utf8');
const MAIN = readFileSync(resolve(VIEW, 'main.legacy.js'), 'utf8');
const HELPER = readFileSync(resolve(process.cwd(), 'src/shared-kit/osfui.js'), 'utf8');

interface Frame {
  type: string;
  payload: Record<string, unknown>;
}

function mount(): { frames: Frame[]; receive(payload: Record<string, unknown>): void } {
  const body = INDEX.match(/<body>([\s\S]*?)<script src=/)?.[1];
  if (!body) throw new Error('handoff body not found');
  document.body.innerHTML = body;

  const frames: Frame[] = [];
  (window as unknown as { osfui: unknown }).osfui = {
    postMessage(json: string) {
      frames.push(JSON.parse(json) as Frame);
    },
  };
  new Function(HELPER)();
  new Function(MAIN)();

  return {
    frames,
    receive(payload) {
      window.osfui!.onMessage!(JSON.stringify({ type: 'handoff.state', payload }));
    },
  };
}

beforeEach(() => {
  document.documentElement.removeAttribute('style');
  document.body.removeAttribute('data-live');
  document.body.removeAttribute('data-phase');
});

describe('first-load handoff surface', () => {
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

    expect(document.body.dataset.live).toBe('true');
    expect(document.body.dataset.phase).toBe('linking');
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

    (document.querySelector('#retry') as HTMLButtonElement).click();
    (document.querySelector('#close') as HTMLButtonElement).click();

    expect(app.frames.map((frame) => frame.payload.command)).toEqual([
      'osfui.handoffRetry',
      'close',
    ]);
  });
});
