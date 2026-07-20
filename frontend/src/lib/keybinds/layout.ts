// layout.ts — the on-screen keyboard layout.
//
// Extracted verbatim from src/views/osfui/keybinds/main.legacy.js:177-200.
//
// A US ANSI keyboard, minus the numpad. `d` is the printed label, `n` is the
// OSF UI key NAME the label maps to, `w` is a width in units (flex-grow, with
// flex-basis 0, so the units are relative within a row).
//
// `n: null` marks a DEAD cell — drawn, but not bindable by mods. Esc is now
// the only one: it IS resolvable, but is reserved because the capture flow
// treats a press of it as "cancel", so binding it would make rebinds
// unescapable.
//
// The punctuation keys (- = [ ] \ ; ' , . /) used to be dead too, because
// native had no name for their VKs. They resolve now (InputRouter.cpp
// KeyName/kNamedKeys), so they are ordinary bindable cells. Their names are the
// US ANSI meanings — the same layout assumption `Grave` has always carried.

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

/** Type guard for the discriminated pair — legacy tested `"gap" in item`. */
export function isGap(item: LayoutItem): item is GapCell {
  return 'gap' in item;
}

/**
 * Bindable cell. `n` defaults to `d` when omitted — the shorthand behind
 * `K("F1")` and `K("Q")`, where label and key name coincide. The default is
 * keyed on `undefined` specifically, so an explicit `null` still makes a dead
 * cell if a caller wants one.
 *
 * `w || 1` means a width of 0 silently becomes 1. Preserved from legacy 182.
 */
export const K = (d: string, n?: string | null, w?: number): KeyCell => ({
  d,
  n: n === undefined ? d : n,
  w: w || 1,
});

/** Non-bindable cell. */
export const DEAD = (d: string, w?: number): KeyCell => ({ d, n: null, w: w || 1 });

/** Spacer. */
export const GAP = (w: number): GapCell => ({ gap: w });

/**
 * Main block: function row, number row, QWERTY, home row, ZXCV, modifier row.
 *
 * The GAPs in the function row reproduce the physical F1-F4 / F5-F8 / F9-F12
 * clusters. Widths are the real ANSI ratios (Tab 1.45, Caps 1.75, LShift 2.25,
 * Enter 2.15, Space 7.3) so the board reads as a keyboard at a glance.
 *
 * Shift/Ctrl/Alt are drawn with the SAME label on both sides but distinct
 * names (LShift/RShift, ...), because native resolves them to distinct VKs
 * and a mod binding one does not get the other.
 */
export const KEYBOARD_MAIN: readonly (readonly LayoutItem[])[] = [
  [DEAD('Esc', 1.2), GAP(0.55), K('F1'), K('F2'), K('F3'), K('F4'), GAP(0.35), K('F5'), K('F6'), K('F7'), K('F8'), GAP(0.35), K('F9'), K('F10'), K('F11'), K('F12')],
  [K('`', 'Grave'), K('1'), K('2'), K('3'), K('4'), K('5'), K('6'), K('7'), K('8'), K('9'), K('0'), K('-', 'Minus'), K('=', 'Equals'), K('Bksp', 'Backspace', 1.9)],
  [K('Tab', 'Tab', 1.45), K('Q'), K('W'), K('E'), K('R'), K('T'), K('Y'), K('U'), K('I'), K('O'), K('P'), K('[', 'LBracket'), K(']', 'RBracket'), K('\\', 'Backslash', 1.45)],
  [K('Caps', 'CapsLock', 1.75), K('A'), K('S'), K('D'), K('F'), K('G'), K('H'), K('J'), K('K'), K('L'), K(';', 'Semicolon'), K("'", 'Apostrophe'), K('Enter', 'Enter', 2.15)],
  [K('Shift', 'LShift', 2.25), K('Z'), K('X'), K('C'), K('V'), K('B'), K('N'), K('M'), K(',', 'Comma'), K('.', 'Period'), K('/', 'Slash'), K('Shift', 'RShift', 2.65)],
  [K('Ctrl', 'LCtrl', 1.4), K('Alt', 'LAlt', 1.4), K('Space', 'Space', 7.3), K('Alt', 'RAlt', 1.4), K('Ctrl', 'RCtrl', 1.4)],
];

/**
 * Navigation block: the 6-key cluster, a spacer row, then the inverted-T
 * arrows. Row 3 is a bare GAP(3) purely to open vertical space between the
 * two clusters; rows 4-5 use GAP(1) shoulders to centre the Up arrow.
 */
export const KEYBOARD_NAV: readonly (readonly LayoutItem[])[] = [
  [K('Ins', 'Insert'), K('Home'), K('PgUp', 'PageUp')],
  [K('Del', 'Delete'), K('End'), K('PgDn', 'PageDown')],
  [GAP(3)],
  [GAP(1), K('↑', 'Up'), GAP(1)],
  [K('←', 'Left'), K('↓', 'Down'), K('→', 'Right')],
];
