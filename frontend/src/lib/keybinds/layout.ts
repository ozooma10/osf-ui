
export interface KeyCell {
  /** Printed label. */
  d: string;
  /** OSF UI key name, or null when the cell is dead. */
  n: string | null;
  /** Width in flex units. */
  w: number;
}

/** A spacer. `gap` is its width in the same flex units as KeyCell.w. */
export interface GapCell {
  gap: number;
}

export type LayoutItem = KeyCell | GapCell;

export function isGap(item: LayoutItem): item is GapCell {
  return 'gap' in item;
}

export const K = (d: string, n?: string | null, w?: number): KeyCell => ({
  d,
  n: n === undefined ? d : n,
  w: w || 1,
});

/** Non-bindable cell. */
export const DEAD = (d: string, w?: number): KeyCell => ({ d, n: null, w: w || 1 });

export const GAP = (w: number): GapCell => ({ gap: w });

export const KEYBOARD_MAIN: readonly (readonly LayoutItem[])[] = [
  [DEAD('Esc', 1.2), GAP(0.55), K('F1'), K('F2'), K('F3'), K('F4'), GAP(0.35), K('F5'), K('F6'), K('F7'), K('F8'), GAP(0.35), K('F9'), K('F10'), K('F11'), K('F12')],
  [K('`', 'Grave'), K('1'), K('2'), K('3'), K('4'), K('5'), K('6'), K('7'), K('8'), K('9'), K('0'), K('-', 'Minus'), K('=', 'Equals'), K('Bksp', 'Backspace', 1.9)],
  [K('Tab', 'Tab', 1.45), K('Q'), K('W'), K('E'), K('R'), K('T'), K('Y'), K('U'), K('I'), K('O'), K('P'), K('[', 'LBracket'), K(']', 'RBracket'), K('\\', 'Backslash', 1.45)],
  [K('Caps', 'CapsLock', 1.75), K('A'), K('S'), K('D'), K('F'), K('G'), K('H'), K('J'), K('K'), K('L'), K(';', 'Semicolon'), K("'", 'Apostrophe'), K('Enter', 'Enter', 2.15)],
  [K('Shift', 'LShift', 2.25), K('Z'), K('X'), K('C'), K('V'), K('B'), K('N'), K('M'), K(',', 'Comma'), K('.', 'Period'), K('/', 'Slash'), K('Shift', 'RShift', 2.65)],
  [K('Ctrl', 'LCtrl', 1.4), K('Alt', 'LAlt', 1.4), K('Space', 'Space', 7.3), K('Alt', 'RAlt', 1.4), K('Ctrl', 'RCtrl', 1.4)],
];

export function mainBlock(hasIntlBackslash: boolean): readonly (readonly LayoutItem[])[] {
  if (!hasIntlBackslash) return KEYBOARD_MAIN;
  return KEYBOARD_MAIN.map((row, index) =>
    index === 4 ? [K('Shift', 'LShift', 1.2), K('\\', 'IntlBackslash', 1.05), ...row.slice(1)] : row,
  );
}

export const KEYBOARD_NAV: readonly (readonly LayoutItem[])[] = [
  [K('Ins', 'Insert'), K('Home'), K('PgUp', 'PageUp')],
  [K('Del', 'Delete'), K('End'), K('PgDn', 'PageDown')],
  [GAP(3)],
  [GAP(1), K('↑', 'Up'), GAP(1)],
  [K('←', 'Left'), K('↓', 'Down'), K('→', 'Right')],
];
