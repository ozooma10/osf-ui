
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
} from '@lib/settings/health';
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
            return null;
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
                      'No mods installed yet. Mods that provide settings, menu views, or HUD views appear here.',
                    )}
              </div>
            );
          case 'entry':
            return (
              <RailItem
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


function railSub(entry: RailEntry, tr: Translator): string {
  if (entry.id === FRAMEWORK_ID) return tr('framework', 'Framework');
  if (entry.mod) return entry.mod.id;
  const menus = entry.views.filter((v) => v.kind === 'menu').length;
  const huds = entry.views.length - menus;
  const parts: string[] = [];
  // Compatibility catalog addresses retain the former terminal/overlay nouns.
  if (menus) parts.push(tr.plural('terminal', menus, 'Menu', '{count} menus'));
  if (huds) parts.push(tr.plural('overlay', huds, 'HUD', '{count} HUDs'));
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
      onClick={() => onSelect(entry.id)}
    >
      <Mark
        class="rail-item-mark"
        iconClass="rail-item-mark--icon"
        src={modIconSrc(entry.mod as Parameters<typeof modIconSrc>[0], assetRoots)}
        color=""
        // Glyph rather than initials: "OU" would read as just another mod.
        fallback={isFramework ? '◆' : initials(entry.title)}
      />
      <span class="rail-item-text">
        <span class="rail-item-title">{entry.title}</span>
        <span class="rail-item-sub">{railSub(entry, tr)}</span>
      </span>
      {severity ? (
        <span
          class={`rail-item-severity rail-item-severity--${severity}`}
          title={
            severity === 'error'
              ? tr('railSeverityError', 'An error affects this mod — see System Health')
              : tr('railSeverityWarning', 'A warning affects this mod — see System Health')
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

/** Fixed rail item — same chrome as a mod entry, selected the same way. */
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
        {/* Compatibility catalog address; fallback copy uses the canonical view noun. */}
        <span class="rail-item-title">{tr('systems', 'Views')}</span>
        <span class="rail-item-sub">
          {views.length
            // Compatibility catalog address; fallback copy uses canonical view kinds.
            ? tr('surfaceCounts', '{menus} menus · {huds} HUDs', { menus, huds })
            : tr('standby', 'Standby')}
        </span>
      </span>
    </button>
  );
}

export interface HealthItemProps {
  health: HealthModel;
  selected: boolean;
  tr: Translator;
  onSelect: (id: string) => void;
}

/** Fixed System Health destination painted above the installed-mod label. */
export function HealthItem({ health, selected, tr, onSelect }: HealthItemProps) {
  const counts = countIssues(health.issues);
  const severity = overallSeverity(counts);
  const classes = [
    'rail-item--health',
    severity ? `rail-item--health-${severity}` : '',
    selected ? 'selected' : '',
  ]
    .filter(Boolean)
    .join(' ');
  const status =
    counts.errors > 0
      ? tr.plural('errorCount', counts.errors, '{count} error', '{count} errors')
      : counts.warnings > 0
        ? tr.plural('warningCount', counts.warnings, '{count} warning', '{count} warnings')
        : tr('nominal', 'Nominal');

  return (
    <button type="button" class={classes} onClick={() => onSelect(HEALTH_ID)}>
      <span class="rail-health-mark" aria-hidden="true">
        {severity === 'error' ? '✕' : severity === 'warning' ? '!' : '✓'}
      </span>
      <span class="rail-health-title">{tr('systemHealth', 'System Health')}</span>
      <span class={`rail-health-status rail-health-status--${severity ?? 'ok'}`}>{status}</span>
      <span class="rail-health-chevron" aria-hidden="true">▸</span>
    </button>
  );
}
