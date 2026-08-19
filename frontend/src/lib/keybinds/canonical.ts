
/** Keys are lowercase; lookup is on `s.toLowerCase()`, so matching is case-insensitive. */
export const NAME_ALIASES: Readonly<Record<string, string>> = {
  tilde: 'Grave',
  backtick: 'Grave',
  console: 'Grave',
  return: 'Enter',
  hyphen: 'Minus',
  dash: 'Minus',
  equal: 'Equals',
  plus: 'Equals',
  leftbracket: 'LBracket',
  rightbracket: 'RBracket',
  quote: 'Apostrophe',
  dot: 'Period',
  // W3C KeyboardEvent.code spellings, accepted as authoring aliases natively.
  backquote: 'Grave',
  bracketleft: 'LBracket',
  bracketright: 'RBracket',
  arrowup: 'Up',
  arrowdown: 'Down',
  arrowleft: 'Left',
  arrowright: 'Right',
  shiftleft: 'LShift',
  shiftright: 'RShift',
  controlleft: 'LCtrl',
  controlright: 'RCtrl',
  altleft: 'LAlt',
  altright: 'RAlt',
  metaleft: 'LWin',
  metaright: 'RWin',
  contextmenu: 'Apps',
  oem102: 'IntlBackslash',
  prtscn: 'PrintScreen',
};

export function canonicalName(name: unknown): string {
  const s = String(name || '');
  const folded = NAME_ALIASES[s.toLowerCase()];
  if (folded) return folded;
  if (/^[a-z]$/.test(s)) return s.toUpperCase(); // letters store uppercase
  return s;
}
