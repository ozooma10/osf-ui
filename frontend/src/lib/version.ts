
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

export function deriveNeedsUpdate(
  osfuiReleaseVersion: string,
  viewTargets: readonly VersionTarget[],
  modTargets: readonly VersionTarget[],
): NeedsUpdate {
  if (!osfuiReleaseVersion) return { outdated: false, wanting: [] };

  const asking = (t: VersionTarget): boolean =>
    !!t.targetVersion && versionLess(osfuiReleaseVersion, t.targetVersion);

  const wanting = [
    ...new Set([
      ...viewTargets.filter(asking).map((t) => t.label),
      ...modTargets.filter(asking).map((t) => t.label),
    ]),
  ];

  return { outdated: wanting.length > 0, wanting };
}
