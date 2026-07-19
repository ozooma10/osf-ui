// canonical.ts — key-name alias folding.
//
// Extracted from src/views/osfui/keybinds/main.legacy.js:63-72.
//
// WHY this exists: the keybinds board groups bindings by KEY NAME STRING, but
// native resolves conflicts by virtual-key code. Several spellings resolve to
// the same VK ("Tilde"/"Backtick"/"Console" are all the Grave key; "Return" is
// Enter). Folding the aliases here makes the JS-side string grouping agree with
// the store's vk-resolved conflict data WITHOUT reimplementing VK resolution in
// the renderer.

/**
 * Alias table. Keys are lowercase because lookup is done on `s.toLowerCase()`,
 * which makes matching case-insensitive: "TILDE", "Tilde" and "tilde" all fold.
 */
export const NAME_ALIASES: Readonly<Record<string, string>> = {
  tilde: 'Grave',
  backtick: 'Grave',
  console: 'Grave',
  return: 'Enter',
};

/**
 * Fold a key name to its canonical KeyName() spelling.
 *
 * Three branches, in the legacy order:
 *   1. An alias hit (case-insensitive) returns the canonical spelling.
 *   2. A single a-z character is uppercased — letters are stored uppercase.
 *   3. Everything else passes through unchanged.
 *
 * QUIRK (legacy line 70): the `/^[a-z]$/` test runs against the ORIGINAL
 * string `s`, not the lowercased one used for the alias lookup. So "A" is
 * already uppercase and falls through branch 3 unchanged (same result), but
 * it also means the rule is genuinely "single LOWERCASE letter" rather than
 * "single letter". Preserved deliberately: `canonicalName("A") === "A"` by
 * passthrough, not by uppercasing.
 *
 * Non-string input is coerced the same way legacy did: `String(name || "")`,
 * so null/undefined/0/"" all become "". Note `String(name || "")` — NOT
 * `String(name)` — is why `0` becomes "" rather than "0".
 *
 * KNOWN BUG, PRESERVED ON PURPOSE: the alias lookup is a bare index into an
 * object literal, so it walks the prototype chain. A name whose LOWERCASING
 * equals an Object.prototype member hits the inherited value, which is truthy,
 * so this returns a non-string despite the `string` return type.
 *
 * The lowercasing narrows the blast radius to the all-lowercase members only:
 * "constructor" (returns the Object constructor FUNCTION) and "__proto__"
 * (returns Object.prototype itself). "toString"/"valueOf"/"hasOwnProperty" are
 * SAFE here precisely because they lowercase to "tostring"/"valueof"/
 * "hasownproperty", which are not members. Downstream, a poisoned value lands
 * in `BindingRow.name` and the first `.toLowerCase()` in the search predicate
 * throws.
 *
 * It is NOT guarded here because unlike domKeyName() this input is REACHABLE:
 * `value` comes from a mod's stored settings values, i.e. arbitrary
 * mod-authored strings. Adding an own-property guard would therefore be an
 * OBSERVABLE behaviour change to a shipped view, which this port does not make
 * unilaterally. (domKeyName() *is* guarded, because its input is confined to
 * the UI Events `key` vocabulary, so the guard changes nothing observable
 * there.) Flagged for a deliberate follow-up fix — the correct repair is
 * `Object.prototype.hasOwnProperty.call(NAME_ALIASES, k)`, or a null-prototype
 * table, applied together with a native-side decision on what an unmappable
 * name should do. Pinned by a test so the quirk cannot vanish silently.
 */
export function canonicalName(name: unknown): string {
  const s = String(name || '');
  const folded = NAME_ALIASES[s.toLowerCase()];
  if (folded) return folded;
  if (/^[a-z]$/.test(s)) return s.toUpperCase(); // letters store uppercase
  return s;
}
