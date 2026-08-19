import { describe, it, expect } from 'vitest';
import { domKeyName } from '@lib/keybinds/domKeyName';

const k = (key: string) => domKeyName({ key });
const c = (code: string) => domKeyName({ key: 'irrelevant', code });

describe('domKeyName', () => {
  it('passes F1-F24 through verbatim', () => {
    for (let i = 1; i <= 24; ++i) expect(k(`F${i}`)).toBe(`F${i}`);
  });

  it('rejects F-keys outside the native range', () => {
    expect(k('F0')).toBe('');
    expect(k('F25')).toBe('');
  });

  it('uppercases letters of EITHER case', () => {
    expect(k('a')).toBe('A');
    expect(k('A')).toBe('A');
    expect(k('z')).toBe('Z');
  });

  it('passes digits through', () => {
    for (const d of '0123456789') expect(k(d)).toBe(d);
  });

  it('maps the named keys', () => {
    expect(k(' ')).toBe('Space');
    expect(k('Enter')).toBe('Enter');
    expect(k('Tab')).toBe('Tab');
    expect(k('Backspace')).toBe('Backspace');
    expect(k('Insert')).toBe('Insert');
    expect(k('Delete')).toBe('Delete');
    expect(k('Home')).toBe('Home');
    expect(k('End')).toBe('End');
    expect(k('PageUp')).toBe('PageUp');
    expect(k('PageDown')).toBe('PageDown');
    expect(k('`')).toBe('Grave');
  });

  it('strips the Arrow prefix', () => {
    expect(k('ArrowUp')).toBe('Up');
    expect(k('ArrowDown')).toBe('Down');
    expect(k('ArrowLeft')).toBe('Left');
    expect(k('ArrowRight')).toBe('Right');
  });

  it('returns "" for unmapped keys, which the capture path treats as cancel', () => {
    expect(k('Escape')).toBe('');
    expect(k('Shift')).toBe('');
    expect(k('Control')).toBe('');
    expect(k('Alt')).toBe('');
    expect(k('CapsLock')).toBe('');
    expect(k('')).toBe('');
  });

  it('names the OEM punctuation keys, which used to be unbindable', () => {
    expect(k('-')).toBe('Minus');
    expect(k('=')).toBe('Equals');
    expect(k('[')).toBe('LBracket');
    expect(k(']')).toBe('RBracket');
    expect(k('\\')).toBe('Backslash');
    expect(k(';')).toBe('Semicolon');
    expect(k("'")).toBe('Apostrophe');
    expect(k(',')).toBe('Comma');
    expect(k('.')).toBe('Period');
    expect(k('/')).toBe('Slash');
  });
});

describe('domKeyName over e.code (physical, layout-independent)', () => {
  it('wins over e.key whenever code is present', () => {
    expect(domKeyName({ key: 'ö', code: 'Semicolon' })).toBe('Semicolon');
    expect(domKeyName({ key: 'z', code: 'KeyY' })).toBe('Y');
  });

  it('strips Key/Digit prefixes and passes F-keys and Numpad digits through', () => {
    expect(c('KeyW')).toBe('W');
    expect(c('Digit1')).toBe('1');
    for (let i = 1; i <= 24; ++i) expect(c(`F${i}`)).toBe(`F${i}`);
    expect(c('Numpad0')).toBe('Numpad0');
    expect(c('Numpad9')).toBe('Numpad9');
  });

  it('maps punctuation, intl and numpad-operator codes to canonical names', () => {
    expect(c('Backquote')).toBe('Grave');
    expect(c('Minus')).toBe('Minus');
    expect(c('Equal')).toBe('Equals');
    expect(c('BracketLeft')).toBe('LBracket');
    expect(c('BracketRight')).toBe('RBracket');
    expect(c('Backslash')).toBe('Backslash');
    expect(c('Semicolon')).toBe('Semicolon');
    expect(c('Quote')).toBe('Apostrophe');
    expect(c('Comma')).toBe('Comma');
    expect(c('Period')).toBe('Period');
    expect(c('Slash')).toBe('Slash');
    expect(c('IntlBackslash')).toBe('IntlBackslash');
    expect(c('NumpadEnter')).toBe('NumpadEnter');
    expect(c('NumpadDivide')).toBe('NumpadDivide');
    expect(c('ContextMenu')).toBe('Apps');
    expect(c('PrintScreen')).toBe('PrintScreen');
  });

  it('resolves SIDED modifiers, which the e.key path never could', () => {
    expect(c('ShiftLeft')).toBe('LShift');
    expect(c('ShiftRight')).toBe('RShift');
    expect(c('ControlLeft')).toBe('LCtrl');
    expect(c('ControlRight')).toBe('RCtrl');
    expect(c('AltLeft')).toBe('LAlt');
    expect(c('AltRight')).toBe('RAlt');
  });

  it('keeps Escape and the Win keys unbindable (capture-reserved natively)', () => {
    expect(c('Escape')).toBe('');
    expect(c('MetaLeft')).toBe('');
    expect(c('MetaRight')).toBe('');
    expect(c('NotACode')).toBe('');
    // Prototype members never leak through the code table.
    expect(c('constructor')).toBe('');
    expect(c('__proto__')).toBe('');
  });

  it('falls back to the e.key branch when code is empty', () => {
    expect(domKeyName({ key: ';', code: '' })).toBe('Semicolon');
  });

  it('does not inherit Object.prototype members through the named table', () => {
    expect(k('constructor')).toBe('');
    expect(k('toString')).toBe('');
  });
});
