// i18n-pseudo.ts — the harness pseudo-locale transform. DEV ONLY.
//
// Ported from devtools/harness/mockbridge.js:168-179.
//
// Accent every letter (glyph coverage), pad ~30% (German-ish expansion), and
// bracket the whole string. The brackets are the point: a bare-English survivor
// on screen is a string that never went through the localization path, and
// anything overflowing its box is a layout that will not survive a real
// translation. Text stays readable throughout, so the page is still usable
// while you audit it.

/**
 * Latin letter -> accented look-alike. Kept as a plain object literal (not a
 * computed range) so the exact glyph set is reviewable: these are the
 * characters the shipped font must cover for pseudo mode to be legible.
 */
const PSEUDO_ACCENTS: Record<string, string> = {
  A: 'Å', B: 'Ɓ', C: 'Ç', D: 'Đ', E: 'É', F: 'Ƒ', G: 'Ĝ', H: 'Ĥ', I: 'Î', J: 'Ĵ', K: 'Ķ', L: 'Ļ', M: 'Ṁ',
  N: 'Ñ', O: 'Ø', P: 'Þ', Q: 'Ǫ', R: 'Ŕ', S: 'Š', T: 'Ŧ', U: 'Û', V: 'Ṽ', W: 'Ŵ', X: 'Ẋ', Y: 'Ý', Z: 'Ž',
  a: 'å', b: 'ƀ', c: 'ç', d: 'đ', e: 'é', f: 'ƒ', g: 'ĝ', h: 'ĥ', i: 'î', j: 'ĵ', k: 'ķ', l: 'ļ', m: 'ṁ',
  n: 'ñ', o: 'ø', p: 'þ', q: 'ǫ', r: 'ŕ', s: 'š', t: 'ŧ', u: 'û', v: 'ṽ', w: 'ŵ', x: 'ẋ', y: 'ý', z: 'ž',
};

/**
 * Pseudo-localize one string.
 *
 * Non-strings and the empty string pass through UNCHANGED — the legacy guard
 * was `if (typeof s !== "string" || !s) return s`, and it matters: bracketing
 * "" would put a stray "[·]" into every empty label slot on the page.
 */
export function pseudoize<T>(s: T): T | string {
  if (typeof s !== 'string' || !s) return s;
  const accented = s.replace(/[A-Za-z]/g, (ch) => PSEUDO_ACCENTS[ch] || ch);
  // Padding is clamped to 12 so a long paragraph does not double in height,
  // and floored at 1 so even a two-character string visibly grows.
  const pad = '·'.repeat(Math.min(12, Math.max(1, Math.round(s.length * 0.3))));
  return '[' + accented + pad + ']';
}
