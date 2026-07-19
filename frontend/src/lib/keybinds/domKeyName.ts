// domKeyName.ts — DOM KeyboardEvent.key -> OSF UI key name.
//
// Extracted from src/views/osfui/keybinds/main.legacy.js:415-425.
//
// STANDALONE PREVIEW ONLY. In game the capture path is native
// (settings.captureKey -> settings.captured), and native names the key from
// its virtual-key code. This mapping exists purely so the view is usable in a
// plain browser with no bridge, which is why it is deliberately partial: it
// covers the keys the on-screen board draws and nothing else.

/** The named-key table. Anything not listed maps to "" (= not bindable). */
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
};

/**
 * Map a keydown event to an OSF UI key name, or "" when the key is not
 * bindable in the preview.
 *
 * Branch order matters and is preserved from legacy:
 *   1. F1-F24 pass through verbatim.
 *   2. A single letter, EITHER case (the `i` flag on line 418), uppercases.
 *      Contrast canonicalName(), which only uppercases lowercase input.
 *   3. A single digit passes through.
 *   4. The named table; miss => "".
 *
 * Only `e.key` is consulted — never `e.code` — so a non-US layout reports the
 * PRODUCED character, not the physical key. That disagrees with native, which
 * is VK-based (physical). Known and accepted: this path never runs in game.
 *
 * NOTE the modifier keys the board draws (LShift/LCtrl/LAlt and their right
 * twins) are absent from every branch, so pressing Shift in the preview yields
 * "" and finishCapture treats it as a cancel. Deliberate — DOM cannot tell the
 * two sides apart from `e.key` alone.
 */
export function domKeyName(e: { key: string }): string {
  const key = e.key;
  if (/^F([1-9]|1[0-9]|2[0-4])$/.test(key)) return key;
  if (/^[a-z]$/i.test(key)) return key.toUpperCase();
  if (/^[0-9]$/.test(key)) return key;
  // DIVERGENCE FROM LEGACY (line 424), deliberate and reported:
  // legacy did a bare `named[e.key] || ""` on an object literal, so a key
  // named after an Object.prototype member ("constructor", "toString", ...)
  // returned the inherited FUNCTION instead of a string. That is unreachable
  // in practice — `KeyboardEvent.key` comes from a fixed UI Events vocabulary
  // and none of those names are in it — but it would violate this function's
  // `string` return type, so the lookup is own-property-guarded here.
  return Object.prototype.hasOwnProperty.call(NAMED, key) ? NAMED[key] || '' : '';
}
