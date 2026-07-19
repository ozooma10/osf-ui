// version.ts — semver-ish comparison and the "needs update" badge derivation.
//
// `targetVersion` (on a view manifest and on a settings schema) is ADVISORY and
// is the only compatibility mechanism left after capabilities/requires were
// removed: nothing is gated on it. Everything still renders best-effort — a
// setting of a type this host predates comes up read-only with its own per-row
// hint — so this drives a note and a badge, never a refusal.
//
// Ported from settings/main.legacy.js:1696-1728.

/**
 * Dotted-version compare, numeric per component, missing parts are 0 — so
 * "1.2" < "1.10.0" as a human expects, not as strings.
 *
 * Quirks, all deliberate and all load-bearing:
 *  - Exactly THREE components are compared. A fourth ("1.0.0.5") is invisible,
 *    so it can never make one version "newer" than another.
 *  - `parseInt` ignores trailing junk, which is how the dev harness's
 *    "1.0.0-mock" still compares sanely against a real "1.0.0".
 *  - `parseInt(...) || 0` maps NaN AND 0 to 0, so a non-numeric component
 *    ("1.x.0") reads as zero rather than poisoning the comparison.
 *  - Equal versions return false: this is strict "less than", not "<=".
 */
export function versionLess(a: string, b: string): boolean {
  const pa = String(a).split('.');
  const pb = String(b).split('.');
  for (let i = 0; i < 3; i++) {
    const x = parseInt(pa[i] ?? '', 10) || 0;
    const y = parseInt(pb[i] ?? '', 10) || 0;
    if (x !== y) return x < y;
  }
  return false;
}

/**
 * One thing that declared a `targetVersion` — a catalog view or a settings mod.
 *
 * `label` is what the tooltip names it by. The settings view derives it
 * differently per source: a view uses its owning mod's title, falling back to
 * the raw manifest `mod` string and then the view id
 * (`homeModCaption(v) || v.mod || v.id`); a settings mod uses `titleOf(mod)`.
 * That resolution needs the live mod list, so it stays at the call site and
 * this module takes the finished label.
 */
export interface VersionTarget {
  /** Manifest/schema `targetVersion`. "" (or absent) means undeclared. */
  readonly targetVersion?: string | undefined;
  readonly label: string;
}

export interface NeedsUpdate {
  /** True when at least one installed thing wants a newer OSF UI. */
  readonly outdated: boolean;
  /** Labels of the things asking, de-duplicated, views before mods. */
  readonly wanting: readonly string[];
}

/**
 * Derive the version badge state for the settings view's rail head.
 *
 * When anything's `targetVersion` is newer than the running OSF UI the badge
 * goes yellow, a tag appears beneath it, and the tooltip names the askers — it
 * is OSF UI ITSELF that needs updating, not the mod.
 *
 * QUIRK PRESERVED: an EMPTY `hostVersion` yields no askers at all
 * (legacy:1714 gates the whole list on `hostVersion ? ... : []`). Note this is
 * NOT what `versionLess("", "1.2")` would say — that is true — so the badge is
 * deliberately suppressed before the `runtime.ready` handshake lands rather
 * than flashing "needs update" against a zero host version. `updateNeedsUpdateBadge`
 * is re-derived on every settings.data / views.data / i18n.data push, so the
 * badge appears as soon as the version is known.
 *
 * @param viewTargets catalog views, taken from the UNFILTERED views.data — a
 *   `hub:false` utility view still gets to ask for a newer host (legacy:1748).
 */
export function deriveNeedsUpdate(
  hostVersion: string,
  viewTargets: readonly VersionTarget[],
  modTargets: readonly VersionTarget[],
): NeedsUpdate {
  if (!hostVersion) return { outdated: false, wanting: [] };

  const asking = (t: VersionTarget): boolean =>
    !!t.targetVersion && versionLess(hostVersion, t.targetVersion);

  // Views first, then mods, de-duplicated by first occurrence — the same order
  // the legacy spread-into-a-Set produces, and the order the tooltip reads in.
  const wanting = [
    ...new Set([
      ...viewTargets.filter(asking).map((t) => t.label),
      ...modTargets.filter(asking).map((t) => t.label),
    ]),
  ];

  return { outdated: wanting.length > 0, wanting };
}
