// Rail.tsx — the left-hand list of things you can configure. The painted order
// comes from @lib/settings/rail's `railNodes` so this file and the LB/RB
// `cycleRail` walk cannot drift apart; the order itself is argued there.
//
// System Health is pinned above everything and never filtered, which looks like
// a bug but isn't: a user typing the name of the mod that failed to load must
// still be able to reach the reason, not be told "No mods match the filter"
// (mcm-design.md §14.2). It replaces the old expanded settings-load alert,
// which could only ever state a filename.
//
// The modified-count badge and the severity marker are both derived from the
// model on every render, so there is no row state to reconcile.

import { modifiedCount } from '@lib/settings/modified';
import { modIconSrc, type AssetRoots } from '@lib/settings/assets';
import {
  FRAMEWORK_ID,
  HEALTH_ID,
  HOME_ID,
  railNodes,
  type ModRecord,
  type RailEntry,
  type ViewRecord,
} from '@lib/settings/rail';
import {
  countIssues,
  overallSeverity,
  severityForMod,
  type HealthModel,
  type Severity,
} from '@lib/settings/diagnostics';
import type { Translator } from '@lib/i18n';
import { initials, Mark } from './marks';

export interface RailProps {
  mods: ModRecord[];
  views: ViewRecord[];
  health: HealthModel;
  /** Pre-trimmed, pre-lowercased — railNodes does not normalise it. */
  query: string;
  selectedId: string | null;
  tr: Translator;
  /** Harness-only icon path overrides. Undefined in production. */
  assetRoots: AssetRoots | undefined;
  onSelect: (id: string) => void;
}

export function Rail(props: RailProps) {
  const { mods, views, health, query, selectedId, tr, assetRoots, onSelect } = props;

  const nodes = railNodes({ mods, views }, query);

  return (
    <nav id="rail-list" class="rail-list" aria-label="Installed mods">
      {nodes.map((node, i) => {
        switch (node.kind) {
          case 'health':
            return (
              <HealthItem
                key="health"
                health={health}
                selected={selectedId === HEALTH_ID}
                tr={tr}
                onSelect={onSelect}
              />
            );
          case 'home':
            return (
              <HomeItem
                key="home"
                views={views}
                selected={selectedId === HOME_ID}
                tr={tr}
                onSelect={onSelect}
              />
            );
          case 'section':
            return (
              <div key="section" class="rail-section">
                {tr('mods', 'Mods')}
              </div>
            );
          case 'empty':
            return (
              <div key="empty" class="rail-empty">
                {node.reason === 'filtered'
                  ? tr('noModsMatch', 'No mods match the filter.')
                  : tr(
                      'noModsInstalled',
                      'No mods installed yet. Mods that register settings, terminals or overlays appear here.',
                    )}
              </div>
            );
          case 'entry':
            return (
              <RailItem
                // Entry ids are unique across the rail (mods by id, view-only
                // entries behind a "view:" prefix), so each item's icon-failed
                // state stays with the right mod when the filter reorders.
                key={node.entry.id}
                entry={node.entry}
                selected={node.entry.id === selectedId}
                severity={severityForMod(
                  health.issues,
                  node.entry.mod ? node.entry.mod.id : node.entry.id,
                  node.entry.views.map((v) => v.id),
                )}
                tr={tr}
                assetRoots={assetRoots}
                onSelect={onSelect}
              />
            );
          default:
            // Unreachable; `i` keeps the key unique if a node kind is ever added.
            return <div key={`x${i}`} />;
        }
      })}
    </nav>
  );
}


/**
 * Sub-line text: "Framework" for OSF UI itself, the mod id for a settings mod,
 * a surface census for a view-only entry (whose id is synthetic, so not worth
 * showing).
 */
function railSub(entry: RailEntry, tr: Translator): string {
  if (entry.id === FRAMEWORK_ID) return tr('framework', 'Framework');
  if (entry.mod) return entry.mod.id;
  const menus = entry.views.filter((v) => v.kind === 'menu').length;
  const huds = entry.views.length - menus;
  const parts: string[] = [];
  if (menus) parts.push(tr.plural('terminal', menus, 'Terminal', '{count} terminals'));
  if (huds) parts.push(tr.plural('overlay', huds, 'Overlay', '{count} overlays'));
  // A view-only entry is built from views, so zero views is unreachable; the
  // fallback is defence only.
  return parts.join(' · ') || tr('mod', 'Mod');
}

interface RailItemProps {
  entry: RailEntry;
  selected: boolean;
  /** Worst active health severity attributable to this entry, or null. */
  severity: Severity | null;
  tr: Translator;
  assetRoots: AssetRoots | undefined;
  onSelect: (id: string) => void;
}

function RailItem({ entry, selected, severity, tr, assetRoots, onSelect }: RailItemProps) {
  const isFramework = entry.id === FRAMEWORK_ID;
  const count = entry.mod ? modifiedCount(entry.mod) : 0;

  return (
    <button
      type="button"
      class={selected ? 'rail-item selected' : 'rail-item'}
      // Nothing reads this attribute any more, but it ships in the DOM today.
      data-mod={entry.id}
      onClick={() => onSelect(entry.id)}
    >
      <Mark
        class="rail-item-mark"
        iconClass="rail-item-mark--icon"
        // The SDK `SettingsSchema` type omits `icon` (advisory field, read as
        // `unknown` by modIconSrc); the cast bridges that without loosening the
        // lib's signature.
        src={modIconSrc(entry.mod as Parameters<typeof modIconSrc>[0], assetRoots)}
        color=""
        // Glyph rather than initials: "OU" would read as just another mod.
        fallback={isFramework ? '◆' : initials(entry.title)}
      />
      <span class="rail-item-text">
        <span class="rail-item-title">{entry.title}</span>
        <span class="rail-item-sub">{railSub(entry, tr)}</span>
      </span>
      {/* The severity marker sits BESIDE the modified-setting count, never
          replacing it: "3 changed" and "something is wrong" are different
          facts and a player needs both. Glyph plus title, so it is not
          colour-only. */}
      {severity ? (
        <span
          class={`rail-item-severity rail-item-severity--${severity}`}
          title={
            severity === 'error'
              ? tr('railSeverityError', 'An error affects this mod — see System health')
              : tr('railSeverityWarning', 'A warning affects this mod — see System health')
          }
        >
          {severity === 'error' ? '✕' : '!'}
        </span>
      ) : null}
      {count ? (
        <span
          class="rail-item-count"
          title={tr('changedCount', '{count} changed from default', { count })}
        >
          {String(count)}
        </span>
      ) : null}
    </button>
  );
}

interface HomeItemProps {
  views: ViewRecord[];
  selected: boolean;
  tr: Translator;
  onSelect: (id: string) => void;
}

/** Pinned rail item — same chrome as a mod entry, selected the same way. */
function HomeItem({ views, selected, tr, onSelect }: HomeItemProps) {
  const menus = views.filter((v) => v.kind === 'menu').length;
  const huds = views.length - menus;
  return (
    <button
      type="button"
      class={selected ? 'rail-item rail-item--home selected' : 'rail-item rail-item--home'}
      onClick={() => onSelect(HOME_ID)}
    >
      <span class="rail-item-mark">◉</span>
      <span class="rail-item-text">
        <span class="rail-item-title">{tr('systems', 'Systems')}</span>
        <span class="rail-item-sub">
          {views.length
            ? tr('surfaceCounts', '{menus} terminals · {huds} overlays', { menus, huds })
            : tr('standby', 'Standby')}
        </span>
      </span>
    </button>
  );
}

interface HealthItemProps {
  health: HealthModel;
  selected: boolean;
  tr: Translator;
  onSelect: (id: string) => void;
}

/**
 * The pinned System Health destination. Same chrome as a mod entry so it reads
 * as a place, not a banner — and it is the persistent notification surface:
 * whatever is wrong keeps a count here until it clears, where a toast would
 * have scrolled away seconds after the player missed it.
 */
function HealthItem({ health, selected, tr, onSelect }: HealthItemProps) {
  const counts = countIssues(health.issues);
  const severity = overallSeverity(counts);
  const badge = severity === 'error' ? counts.errors : severity === 'warning' ? counts.warnings : 0;

  const classes = [
    'rail-item',
    'rail-item--health',
    severity ? `rail-item--health-${severity}` : '',
    selected ? 'selected' : '',
  ]
    .filter(Boolean)
    .join(' ');

  return (
    <button type="button" class={classes} onClick={() => onSelect(HEALTH_ID)}>
      <span class="rail-item-mark" aria-hidden="true">
        {severity === 'error' ? '✕' : severity === 'warning' ? '!' : '✓'}
      </span>
      <span class="rail-item-text">
        <span class="rail-item-title">{tr('systemHealth', 'System health')}</span>
        {/* The sub-line states the severity in words — the badge's colour is
            never the only carrier. */}
        <span class="rail-item-sub">
          {severity === 'error'
            ? tr('actionRequired', 'Action required')
            : severity === 'warning'
              ? tr('warningsDetected', 'Warnings detected')
              : tr('nominal', 'Nominal')}
        </span>
      </span>
      {badge ? (
        <span class={`rail-item-count rail-item-count--${severity}`}>{String(badge)}</span>
      ) : null}
    </button>
  );
}
