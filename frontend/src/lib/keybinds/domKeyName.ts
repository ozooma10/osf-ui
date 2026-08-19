
/** W3C KeyboardEvent.code -> OSF UI key name. Missing => not bindable. */
const CODES: Readonly<Record<string, string>> = {
  Space: 'Space',
  Enter: 'Enter',
  Tab: 'Tab',
  Backspace: 'Backspace',
  CapsLock: 'CapsLock',
  Insert: 'Insert',
  Delete: 'Delete',
  Home: 'Home',
  End: 'End',
  PageUp: 'PageUp',
  PageDown: 'PageDown',
  ArrowUp: 'Up',
  ArrowDown: 'Down',
  ArrowLeft: 'Left',
  ArrowRight: 'Right',
  ShiftLeft: 'LShift',
  ShiftRight: 'RShift',
  ControlLeft: 'LCtrl',
  ControlRight: 'RCtrl',
  AltLeft: 'LAlt',
  AltRight: 'RAlt',
  Backquote: 'Grave',
  Minus: 'Minus',
  Equal: 'Equals',
  BracketLeft: 'LBracket',
  BracketRight: 'RBracket',
  Backslash: 'Backslash',
  Semicolon: 'Semicolon',
  Quote: 'Apostrophe',
  Comma: 'Comma',
  Period: 'Period',
  Slash: 'Slash',
  IntlBackslash: 'IntlBackslash',
  IntlRo: 'IntlRo',
  IntlYen: 'IntlYen',
  NumLock: 'NumLock',
  ScrollLock: 'ScrollLock',
  Pause: 'Pause',
  PrintScreen: 'PrintScreen',
  ContextMenu: 'Apps',
  NumpadEnter: 'NumpadEnter',
  NumpadDivide: 'NumpadDivide',
  NumpadMultiply: 'NumpadMultiply',
  NumpadSubtract: 'NumpadSubtract',
  NumpadAdd: 'NumpadAdd',
  NumpadDecimal: 'NumpadDecimal',
};

/** Legacy named-key table for the `e.key` fallback. */
const NAMED: Readonly<Record<string, string>> = {
  ' ': 'Space',
  Enter: 'Enter',
  Tab: 'Tab',
  Backspace: 'Backspace',
  Insert: 'Insert',
  Delete: 'Delete',
  Home: 'Home',
  End: 'End',
  PageUp: 'PageUp',
  PageDown: 'PageDown',
  ArrowUp: 'Up',
  ArrowDown: 'Down',
  ArrowLeft: 'Left',
  ArrowRight: 'Right',
  '`': 'Grave',
  '-': 'Minus',
  '=': 'Equals',
  '[': 'LBracket',
  ']': 'RBracket',
  '\\': 'Backslash',
  ';': 'Semicolon',
  "'": 'Apostrophe',
  ',': 'Comma',
  '.': 'Period',
  '/': 'Slash',
};

const F_KEY = /^F([1-9]|1[0-9]|2[0-4])$/;

export function domKeyName(e: { key: string; code?: string }): string {
  const code = e.code;
  if (typeof code === 'string' && code) {
    if (/^Key[A-Z]$/.test(code)) return code.slice(3);
    if (/^Digit[0-9]$/.test(code)) return code.slice(5);
    if (/^Numpad[0-9]$/.test(code)) return code;
    if (F_KEY.test(code)) return code;
    return Object.prototype.hasOwnProperty.call(CODES, code) ? CODES[code] || '' : '';
  }

  const key = e.key;
  if (F_KEY.test(key)) return key;
  if (/^[a-z]$/i.test(key)) return key.toUpperCase();
  if (/^[0-9]$/.test(key)) return key;
  return Object.prototype.hasOwnProperty.call(NAMED, key) ? NAMED[key] || '' : '';
}
