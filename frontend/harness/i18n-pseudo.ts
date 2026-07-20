// Harness pseudo-locale transform. Dev only.
//
// Accent every letter (glyph coverage), pad ~30% (German-ish expansion), and
// bracket the string. The brackets are the point: unbracketed text on screen is
// a string that never went through the localization path, and anything
// overflowing its box will not survive a real translation.

/**
 * Latin letter -> accented look-alike. A literal rather than a computed range
 * so the glyph set stays reviewable: the shipped font must cover exactly these
 * for pseudo mode to be legible.
 */
const PSEUDO_ACCENTS: Record<string, string> = {
  A: 'Å', B: 'Ɓ', C: 'Ç', D: 'Đ', E: 'É', F: 'Ƒ', G: 'Ĝ', H: 'Ĥ', I: 'Î', J: 'Ĵ', K: 'Ķ', L: 'Ļ', M: 'Ṁ',
  N: 'Ñ', O: 'Ø', P: 'Þ', Q: 'Ǫ', R: 'Ŕ', S: 'Š', T: 'Ŧ', U: 'Û', V: 'Ṽ', W: 'Ŵ', X: 'Ẋ', Y: 'Ý', Z: 'Ž',
  a: 'å', b: 'ƀ', c: 'ç', d: 'đ', e: 'é', f: 'ƒ', g: 'ĝ', h: 'ĥ', i: 'î', j: 'ĵ', k: 'ķ', l: 'ļ', m: 'ṁ',
  n: 'ñ', o: 'ø', p: 'þ', q: 'ǫ', r: 'ŕ', s: 'š', t: 'ŧ', u: 'û', v: 'ṽ', w: 'ŵ', x: 'ẋ', y: 'ý', z: 'ž',
};

/**
 * Pseudo-localize one string. Non-strings and the empty string pass through
 * unchanged: bracketing "" would put a stray "[·]" in every empty label slot.
 */
export function pseudoize<T>(s: T): T | string {
  if (typeof s !== 'string' || !s) return s;
  const accented = s.replace(/[A-Za-z]/g, (ch) => PSEUDO_ACCENTS[ch] || ch);
  // Padding is clamped to 12 so a long paragraph does not double in height,
  // and floored at 1 so even a two-character string visibly grows.
  const pad = '·'.repeat(Math.min(12, Math.max(1, Math.round(s.length * 0.3))));
  return '[' + accented + pad + ']';
}
