// Key-name alias folding.
//
// The keybinds board groups bindings by key-name string, but native resolves
// conflicts by virtual-key code, and several spellings share a VK
// ("Tilde"/"Backtick"/"Console" are all Grave; "Return" is Enter). Folding here
// makes the string grouping agree with the store's vk-resolved conflict data
// without reimplementing VK resolution in the renderer.

/** Keys are lowercase; lookup is on `s.toLowerCase()`, so matching is case-insensitive. */
export const NAME_ALIASES: Readonly<Record<string, string>> = {
  tilde: 'Grave',
  backtick: 'Grave',
  console: 'Grave',
  return: 'Enter',
  // OEM punctuation aliases, mirroring kNamedKeys in InputRouter.cpp. A mod that
  // declares "Quote" or "Dash" in its schema resolves to the same VK natively,
  // so the board must fold them to the same cell or one binding appears twice
  // under two names.
  hyphen: 'Minus',
  dash: 'Minus',
  equal: 'Equals',
  plus: 'Equals',
  leftbracket: 'LBracket',
  rightbracket: 'RBracket',
  quote: 'Apostrophe',
  dot: 'Period',
};

/**
 * Fold a key name to its canonical KeyName() spelling: alias hit
 * (case-insensitive), else a single a-z character uppercased, else unchanged.
 *
 * The `/^[a-z]$/` test runs against the original string, not the lowercased one
 * used for the alias lookup, so the rule is "single lowercase letter":
 * `canonicalName("A") === "A"` by passthrough, not by uppercasing.
 *
 * `String(name || '')` — not `String(name)` — so null/undefined/0/'' all become
 * ''; `0` becomes '' rather than '0'.
 *
 * Known bug, kept on purpose: the alias lookup is a bare index into an object
 * literal, so it walks the prototype chain and returns a non-string despite the
 * `string` return type. The lowercasing limits this to the all-lowercase
 * members: "constructor" (returns the Object constructor) and "__proto__"
 * (returns Object.prototype). "toString"/"valueOf"/"hasOwnProperty" are safe
 * because they lowercase to non-members. A poisoned value lands in
 * `BindingRow.name` and the first `.toLowerCase()` in the search predicate
 * throws.
 *
 * Not guarded here because, unlike domKeyName(), this input is reachable:
 * `value` comes from a mod's stored settings values, i.e. arbitrary
 * mod-authored strings, so a guard would be an observable behaviour change to a
 * shipped view. (domKeyName() is guarded — its input is confined to the UI
 * Events `key` vocabulary, where the guard changes nothing.) The fix is
 * `Object.prototype.hasOwnProperty.call(NAME_ALIASES, k)` or a null-prototype
 * table, together with a native-side decision on what an unmappable name should
 * do. Pinned by a test so the quirk cannot vanish silently.
 */
export function canonicalName(name: unknown): string {
  const s = String(name || '');
  const folded = NAME_ALIASES[s.toLowerCase()];
  if (folded) return folded;
  if (/^[a-z]$/.test(s)) return s.toUpperCase(); // letters store uppercase
  return s;
}
