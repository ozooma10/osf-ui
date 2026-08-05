// Semver-ish comparison and the "needs update" badge derivation.
//
// This module owns only the newer-than-host advisory badge. Explicitly pre-2.0
// view manifests are refused natively and reported through System Health;
// settings schemas continue to load best-effort.

/**
 * Dotted-version compare, numeric per component, missing parts are 0 — so
 * "1.2" < "1.10.0" as a human expects, not as strings.
 *
 * Load-bearing quirks:
 *  - Exactly three components are compared. A fourth ("1.0.0.5") is invisible,
 *    so it can never make one version newer than another.
 *  - `parseInt` ignores trailing junk, which is how the dev harness's
 *    "1.0.0-mock" still compares sanely against a real "1.0.0".
 *  - `parseInt(...) || 0` maps NaN and 0 to 0, so a non-numeric component
 *    ("1.x.0") reads as zero rather than poisoning the comparison.
 *  - Equal versions return false: strict "less than", not "<=".
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
 * `label` is what the tooltip names it by, resolved at the call site because it
 * needs the live mod list: views use `homeModCaption(v) || v.mod || v.id`,
 * settings mods use `titleOf(mod)`.
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
 * Version badge state for the settings view's rail head. When anything's
 * `targetVersion` is newer than the running OSF UI the badge goes yellow, a tag
 * appears beneath it, and the tooltip names the askers — it is OSF UI itself
 * that needs updating, not the mod.
 *
 * An empty `hostVersion` yields no askers, overriding `versionLess("", "1.2")`
 * (which is true): the badge stays suppressed before the `runtime.ready`
 * handshake lands rather than flashing "needs update" against a zero host
 * version. The badge is re-derived on every settings.data / views.data /
 * i18n.data push, so it appears as soon as the version is known.
 *
 * @param viewTargets catalog views from the unfiltered views.data — a
 *   `hub:false` utility view still gets to ask for a newer host.
 */
export function deriveNeedsUpdate(
  hostVersion: string,
  viewTargets: readonly VersionTarget[],
  modTargets: readonly VersionTarget[],
): NeedsUpdate {
  if (!hostVersion) return { outdated: false, wanting: [] };

  const asking = (t: VersionTarget): boolean =>
    !!t.targetVersion && versionLess(hostVersion, t.targetVersion);

  // Views first, then mods, de-duplicated by first occurrence — the order the
  // tooltip reads in.
  const wanting = [
    ...new Set([
      ...viewTargets.filter(asking).map((t) => t.label),
      ...modTargets.filter(asking).map((t) => t.label),
    ]),
  ];

  return { outdated: wanting.length > 0, wanting };
}
