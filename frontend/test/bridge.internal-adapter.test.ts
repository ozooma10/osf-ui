// @vitest-environment jsdom

import { afterEach, describe, expect, it, vi } from 'vitest';
import { windowBridge } from '@lib/bridge';

type StateHandler = (value: unknown) => void;

function installBridge(): { emitState(value: unknown): void } {
  let stateHandler: StateHandler | undefined;
  window.osfui = {
    postMessage: vi.fn(),
    send: vi.fn(() => true),
    request: vi.fn(() => Promise.resolve(undefined)),
    on: vi.fn(() => () => {}),
    state: {
      get: vi.fn(() => undefined),
      on: vi.fn((_key: string, fn: StateHandler) => {
        stateHandler = fn;
        return () => {};
      }),
    },
  };

  return {
    emitState(value) {
      stateHandler?.(value);
    },
  };
}

afterEach(() => {
  delete window.osfui;
  document.documentElement.lang = '';
});

describe('built-in bridge adapter', () => {
  it('keeps localization private while consuming the platform catalog state', () => {
    const bridge = installBridge();
    const consumer = vi.fn();

    windowBridge.state('osfui/i18n', consumer);
    bridge.emitState({
      locale: 'fr',
      strings: { 'settings.title': 'Paramètres pour {name}' },
    });

    expect(consumer).toHaveBeenCalledWith({
      locale: 'fr',
      strings: { 'settings.title': 'Paramètres pour {name}' },
    });
    expect(windowBridge.t('settings.title', 'Settings for {name}', { name: 'OSF UI' }))
      .toBe('Paramètres pour OSF UI');
    expect(document.documentElement.lang).toBe('fr');
    expect(window.osfui).not.toHaveProperty('i18n');
    expect(window.osfui).not.toHaveProperty('theme');
  });

  it('derives private accent tokens without adding a public theme helper', () => {
    installBridge();
    const element = document.createElement('div');

    windowBridge.applyAccent(element, '#336699');

    expect(element.style.getPropertyValue('--osf-accent')).toBe('#336699');
    expect(element.style.getPropertyValue('--osf-accent-hover')).toBe('#789abc');
    expect(element.style.getPropertyValue('--osf-accent-strong')).toBe('#1e3b59');
    expect(element.style.getPropertyValue('--osf-accent-quiet')).toBe('rgba(51, 102, 153, 0.14)');
    expect(window.osfui).not.toHaveProperty('theme');
  });
});
