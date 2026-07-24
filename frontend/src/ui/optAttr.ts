/**
 * One conditional DOM attribute, as a spreadable object: `{...optAttr(n, v)}`.
 *
 * Emits nothing when the value is falsy. Present-but-empty is not the same as
 * absent, and the difference is per-attribute:
 *  * `data-key=""` still MATCHES a `[data-key]` selector, which is written
 *    expecting a real key — this is the one that would actually break something
 *    (settings' search-jump scans `.row[data-key]`).
 *  * `title=""` does NOT render a blank tooltip — browsers show none. It does
 *    something subtler: `title` is inherited by descendants from the nearest
 *    ancestor that has one, and an empty `title=""` explicitly means "no
 *    advisory text", so it SUPPRESSES an ancestor's tooltip on this subtree.
 *
 * ## The kit's required-with-"" convention
 *
 * `exactOptionalPropertyTypes` forbids passing an explicit `undefined` where the
 * ABSENCE of a property is what carries the meaning. So kit components declare
 * these as REQUIRED props taking `""` for "none" rather than optional ones, and
 * drop them at the point of use. That convention covers both kinds: class props
 * folded in by `cx()` (`Badge.modifier`, `Row.class` — where `""` simply
 * contributes nothing) and attribute props dropped by this helper.
 *
 * ## Cost, accepted deliberately
 *
 * The return is an index-signature type, so spreading it gives up JSX's checking
 * of BOTH the attribute name and its value type against the element — a typo'd
 * name or a wrong-typed value compiles clean here where a literal attribute
 * would not. That is tolerable for the plain-string attributes it is used for.
 * Do NOT reach for it with event handlers or `style` objects: those have precise
 * types worth keeping, and they should be spread inline instead.
 */
export function optAttr<T>(
  name: string,
  value: T | undefined | null | false | '',
): Record<string, T> {
  return value ? { [name]: value as T } : {};
}
