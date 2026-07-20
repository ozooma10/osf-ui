import { describe, it, expect } from 'vitest';
import { makeTranslator, isAbsoluteAddress, type TranslatorHost } from '@lib/i18n';
import { nullBridge } from '@lib/bridge';

/**
 * Stand-in for the frozen `osfui.t` in shared-kit/osfui.js: catalog hit or
 * authored English, then `{name}` interpolation over the result. Records every
 * address it was asked for — that is what the prefixing assertions check.
 */
function catalogHost(strings: Record<string, string> = {}): TranslatorHost & {
  asked: string[];
} {
  const asked: string[] = [];
  return {
    asked,
    t(address, english, vars) {
      asked.push(address);
      const value = Object.prototype.hasOwnProperty.call(strings, address)
        ? strings[address]
        : english;
      return String(value ?? '').replace(/\{([A-Za-z0-9_]+)\}/g, (all, name: string) =>
        vars && Object.prototype.hasOwnProperty.call(vars, name) ? String(vars[name]) : all,
      );
    },
  };
}

describe('isAbsoluteAddress', () => {
  it('treats any dotted address as absolute and bare tokens as relative', () => {
    expect(isAbsoluteAddress('chrome.common.loading')).toBe(true);
    expect(isAbsoluteAddress('a.b')).toBe(true);
    expect(isAbsoluteAddress('writeRejected')).toBe(false);
    expect(isAbsoluteAddress('')).toBe(false);
  });
});

describe('makeTranslator address resolution', () => {
  it('prefixes a bare token with the view namespace', () => {
    const host = catalogHost();
    const tr = makeTranslator(host, 'chrome.settings.');
    tr('writeRejected', 'Rejected');
    expect(host.asked).toEqual(['chrome.settings.writeRejected']);
  });

  it('accepts a prefix with or without its trailing dot', () => {
    const host = catalogHost();
    makeTranslator(host, 'chrome.keybinds')('rebind', 'Rebind');
    makeTranslator(host, 'chrome.keybinds.')('rebind', 'Rebind');
    expect(host.asked).toEqual(['chrome.keybinds.rebind', 'chrome.keybinds.rebind']);
  });

  it('passes a dotted (absolute) address through UNPREFIXED', () => {
    // Blind prefixing would ask for "chrome.keybinds.chrome.common.loading",
    // which no catalog carries.
    const host = catalogHost({ 'chrome.common.loading': 'Chargement…' });
    const tr = makeTranslator(host, 'chrome.keybinds.');
    expect(tr('chrome.common.loading', 'Loading…')).toBe('Chargement…');
    expect(host.asked).toEqual(['chrome.common.loading']);
  });

  it('resolves a catalog override for a prefixed address', () => {
    const host = catalogHost({ 'chrome.settings.saved': 'Gespeichert' });
    expect(makeTranslator(host, 'chrome.settings.')('saved', 'Saved')).toBe('Gespeichert');
  });

  it('makes every address absolute when the prefix is empty', () => {
    const host = catalogHost();
    makeTranslator(host, '')('saved', 'Saved');
    expect(host.asked).toEqual(['saved']);
  });
});

describe('interpolation', () => {
  it('substitutes {name} placeholders', () => {
    const tr = makeTranslator(catalogHost(), 'chrome.settings.');
    expect(tr('modWithId', 'Mod · {id}', { id: 'acme.atlas' })).toBe('Mod · acme.atlas');
  });

  it('coerces numeric vars to strings', () => {
    const tr = makeTranslator(catalogHost(), 'chrome.settings.');
    expect(tr('changedCount', '{count} changed from default', { count: 3 })).toBe(
      '3 changed from default',
    );
  });

  it('leaves an unmatched placeholder LITERAL rather than blanking it', () => {
    const tr = makeTranslator(catalogHost(), 'chrome.settings.');
    // Only names present in `vars` are replaced; anything else survives
    // verbatim, so a stale catalog string degrades visibly instead of
    // silently losing text.
    expect(tr('alsoBoundBy', 'Also bound by: {others}')).toBe('Also bound by: {others}');
    expect(tr('x', '{a} and {b}', { a: '1' })).toBe('1 and {b}');
  });

  it('ignores placeholders whose names fall outside [A-Za-z0-9_]', () => {
    const tr = makeTranslator(catalogHost(), 'chrome.settings.');
    expect(tr('x', '{a-b} {c.d}', { 'a-b': 'no', 'c.d': 'no' })).toBe('{a-b} {c.d}');
  });

  it('interpolates a CATALOG string, not just the authored English', () => {
    const host = catalogHost({ 'chrome.settings.changedCount': '{count} geändert' });
    expect(makeTranslator(host, 'chrome.settings.')('changedCount', '{count} changed', { count: 2 }))
      .toBe('2 geändert');
  });
});

describe('English fallback', () => {
  it('falls back to the authored English through nullBridge (no helper present)', () => {
    const tr = makeTranslator(nullBridge, 'chrome.settings.');
    expect(tr('saved', 'Saved')).toBe('Saved');
    expect(tr('changedCount', '{count} changed from default', { count: 7 })).toBe(
      '7 changed from default',
    );
  });

  it('falls back for an absolute address too', () => {
    expect(makeTranslator(nullBridge, 'chrome.keybinds.')('chrome.common.loading', 'Loading…')).toBe(
      'Loading…',
    );
  });
});

describe('two-form plural selection', () => {
  it('selects the One form by address suffix when count === 1', () => {
    const host = catalogHost();
    const tr = makeTranslator(host, 'chrome.settings.');
    expect(tr.plural('presetApplied', 1, 'Applied ({count} setting)', 'Applied ({count} settings)'))
      .toBe('Applied (1 setting)');
    expect(host.asked).toEqual(['chrome.settings.presetAppliedOne']);
  });

  it('selects the Other form for any count that is not exactly 1', () => {
    const host = catalogHost();
    const tr = makeTranslator(host, 'chrome.settings.');
    expect(tr.plural('terminal', 3, 'Terminal', '{count} terminals')).toBe('3 terminals');
    // Zero and negatives take Other — English-shaped, not CLDR.
    expect(tr.plural('terminal', 0, 'Terminal', '{count} terminals')).toBe('0 terminals');
    expect(host.asked).toEqual(['chrome.settings.terminalOther', 'chrome.settings.terminalOther']);
  });

  it('merges {count} in automatically but lets an explicit var win', () => {
    const tr = makeTranslator(catalogHost(), 'chrome.keybinds.');
    expect(tr.plural('bindingCount', 2, '{count} binding', '{count} bindings')).toBe('2 bindings');
    expect(tr.plural('bindingCount', 2, '{count} binding', '{count} bindings', { count: 'two' }))
      .toBe('two bindings');
  });

  it('carries additional vars alongside count', () => {
    const tr = makeTranslator(catalogHost(), 'chrome.settings.');
    expect(
      tr.plural('presetApplied', 2, '"{preset}" ({count})', '"{preset}" ({count})', {
        preset: 'Balanced',
      }),
    ).toBe('"Balanced" (2)');
  });

  it('honours a catalog override on the selected form only', () => {
    const host = catalogHost({ 'chrome.settings.loadErrorOne': 'Eine Datei' });
    const tr = makeTranslator(host, 'chrome.settings.');
    expect(tr.plural('loadError', 1, '1 file', '{count} files')).toBe('Eine Datei');
    expect(tr.plural('loadError', 4, '1 file', '{count} files')).toBe('4 files');
  });
});
