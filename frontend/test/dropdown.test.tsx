// @vitest-environment jsdom

import { afterEach, describe, expect, it } from 'vitest';
import { render } from 'preact';
import { act } from 'preact/test-utils';
import { Dropdown, dropdownPlacement } from '@ui/Dropdown';

const OPTIONS = [
  { value: 'all', label: 'All' },
  { value: 'active', label: 'Active now' },
  { value: 'gameplay', label: 'Gameplay' },
] as const;

let host: HTMLElement | null = null;

function mount(onCommit: (value: string) => void = () => {}) {
  host = document.createElement('div');
  document.body.appendChild(host);
  act(() => {
    render(
      <Dropdown
        id="filter"
        value="active"
        options={OPTIONS}
        disabled={false}
        ariaLabel="Filter bindings"
        onCommit={onCommit}
      />,
      host as HTMLElement,
    );
  });
  const trigger = host.querySelector<HTMLButtonElement>('#filter')!;
  trigger.getBoundingClientRect = () => ({
    left: 700,
    right: 860,
    top: 650,
    bottom: 682,
    width: 160,
    height: 32,
    x: 700,
    y: 650,
    toJSON: () => ({}),
  });
  return trigger;
}

function key(target: HTMLElement, name: string) {
  act(() => {
    target.dispatchEvent(new KeyboardEvent('keydown', { key: name, bubbles: true, cancelable: true }));
  });
}

afterEach(() => {
  if (host) {
    render(null, host);
    host.remove();
    host = null;
  }
  document.body.innerHTML = '';
});

describe('Dropdown placement', () => {
  it('flips above a low trigger and clamps the menu inside the viewport', () => {
    expect(dropdownPlacement(
      { left: 930, right: 1050, top: 700, bottom: 732, width: 120 },
      1000,
      768,
      320,
    )).toEqual({
      left: 872,
      top: 376,
      width: 120,
      maxHeight: 320,
      opensUp: true,
    });
  });

  it('opens downward and limits height when that is the larger side', () => {
    expect(dropdownPlacement(
      { left: 20, right: 180, top: 80, bottom: 112, width: 160 },
      1000,
      300,
      320,
    )).toEqual({
      left: 20,
      top: 116,
      width: 160,
      maxHeight: 176,
      opensUp: false,
    });
  });
});

describe('Dropdown interaction', () => {
  it('renders a DOM listbox instead of a native select and exposes combobox state', () => {
    const trigger = mount();
    expect(host!.querySelector('select')).toBeNull();
    expect(trigger.getAttribute('role')).toBe('combobox');
    expect(trigger.getAttribute('aria-expanded')).toBe('false');
    expect(trigger.textContent).toContain('Active now');

    act(() => trigger.click());
    const menu = document.querySelector<HTMLElement>('#filter-listbox')!;
    expect(menu).not.toBeNull();
    expect(menu.getAttribute('role')).toBe('listbox');
    expect(menu.getAttribute('data-placement')).toBe('up');
    expect(menu.getAttribute('data-nav-modal')).toBe('1');
    expect(trigger.getAttribute('aria-expanded')).toBe('true');
    expect(trigger.getAttribute('aria-activedescendant')).toBe('filter-option-1');
  });

  it('moves with arrows, commits with Enter, and closes without leaving WebView focus', () => {
    const committed: string[] = [];
    const trigger = mount((value) => {
      committed.push(value);
    });
    trigger.focus();
    act(() => trigger.click());

    key(trigger, 'ArrowDown');
    expect(trigger.getAttribute('aria-activedescendant')).toBe('filter-option-2');
    key(trigger, 'Enter');

    expect(committed).toEqual(['gameplay']);
    expect(document.querySelector('#filter-listbox')).toBeNull();
    expect(document.activeElement).toBe(trigger);
  });

  it('uses Escape for the dropdown rather than allowing it to close the surface', () => {
    const trigger = mount();
    trigger.focus();
    act(() => trigger.click());

    const event = new KeyboardEvent('keydown', { key: 'Escape', bubbles: true, cancelable: true });
    act(() => {
      trigger.dispatchEvent(event);
    });

    expect(event.defaultPrevented).toBe(true);
    expect(document.querySelector('#filter-listbox')).toBeNull();
    expect(document.activeElement).toBe(trigger);
  });
});
