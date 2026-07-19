// Rail.tsx — the left-hand list of things you can configure.
//
// Ports `renderRail` and its helpers (settings/main.legacy.js:747-927), reading
// the painted ORDER from @lib/settings/rail's `railNodes` so that this file and
// the LB/RB `cycleRail` walk cannot drift apart.
//
// The order itself is argued in railNodes' doc comment. The one rule worth
// repeating here because it looks like a bug: THE LOAD-FAILURE ALERT IS NEVER
// FILTERED. A user typing the name of the mod that failed to load must see WHY
// it is missing, not "No mods match the filter" (mcm-design.md §14.2 — the
// SkyUI-MCM lesson: a mod silently absent from the list is a support thread, a
// named file plus a reason is a fix).
//
// `liveRows`/`updateRailCounts` are gone: the modified-count badge is derived
// from the model on every render now, so there is nothing to reconcile.

import { modifiedCount } from '@lib/settings/modified';
import { modIconSrc, type AssetRoots } from '@lib/settings/assets';
import {
  FRAMEWORK_ID,
  HOME_ID,
  railNodes,
  type LoadError,
  type ModRecord,
  type RailEntry,
  type ViewRecord,
} from '@lib/settings/rail';
import type { Translator } from '@lib/i18n';
import { initials, Mark } from './marks';

export interface RailProps {
  mods: ModRecord[];
  views: ViewRecord[];
  loadErrors: LoadError[];
  /** Pre-trimmed, pre-lowercased — railNodes does not normalise it. */
  query: string;
  selectedId: string | null;
  tr: Translator;
  /** Harness-only icon path overrides. Undefined in production. */
  assetRoots: AssetRoots | undefined;
  onSelect: (id: string) => void;
}

export function Rail(props: RailProps) {
  const { mods, views, loadErrors, query, selectedId, tr, assetRoots, onSelect } = props;

  const nodes = railNodes({ mods, views, loadErrors }, query);

  return (
    <nav id="rail-list" class="rail-list" aria-label="Installed mods">
      {nodes.map((node, i) => {
        switch (node.kind) {
          case 'loadErrors':
            return <LoadAlert key="alert" errors={node.errors} tr={tr} />;
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
                // The entry id is unique across the rail (mods by id, view-only
                // entries behind a "view:" prefix), so it is a real identity —
                // which keeps each item's icon-failed state attached to the
                // right mod when the filter reorders the list.
                key={node.entry.id}
                entry={node.entry}
                selected={node.entry.id === selectedId}
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

// ---------------------------------------------------------------------------

/**
 * What the entry OFFERS, for the sub-line (main.legacy.js:787-796): "Framework"
 * for OSF UI itself, the mod id for a settings mod, and a surface census for a
 * view-only entry (which has no id worth showing — its id is synthetic).
 */
function railSub(entry: RailEntry, tr: Translator): string {
  if (entry.id === FRAMEWORK_ID) return tr('framework', 'Framework');
  if (entry.mod) return entry.mod.id;
  const menus = entry.views.filter((v) => v.kind === 'menu').length;
  const huds = entry.views.length - menus;
  const parts: string[] = [];
  if (menus) parts.push(tr.plural('terminal', menus, 'Terminal', '{count} terminals'));
  if (huds) parts.push(tr.plural('overlay', huds, 'Overlay', '{count} overlays'));
  // A view-only entry with zero views cannot exist (it is built FROM views), so
  // the fallback is unreachable defence carried over from legacy.
  return parts.join(' · ') || tr('mod', 'Mod');
}

interface RailItemProps {
  entry: RailEntry;
  selected: boolean;
  tr: Translator;
  assetRoots: AssetRoots | undefined;
  onSelect: (id: string) => void;
}

function RailItem({ entry, selected, tr, assetRoots, onSelect }: RailItemProps) {
  const isFramework = entry.id === FRAMEWORK_ID;
  const count = entry.mod ? modifiedCount(entry.mod) : 0;

  return (
    <button
      type="button"
      class={selected ? 'rail-item selected' : 'rail-item'}
      // Legacy re-found each item by this attribute to repaint its badge
      // (main.legacy.js:1462-1463). Nothing needs it now, but it ships today.
      data-mod={entry.id}
      onClick={() => onSelect(entry.id)}
    >
      <Mark
        class="rail-item-mark"
        iconClass="rail-item-mark--icon"
        src={modIconSrc(entry.mod, assetRoots)}
        color=""
        // The framework gets a glyph rather than initials — "OU" would read as
        // just another mod.
        fallback={isFramework ? '◆' : initials(entry.title)}
      />
      <span class="rail-item-text">
        <span class="rail-item-title">{entry.title}</span>
        <span class="rail-item-sub">{railSub(entry, tr)}</span>
      </span>
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

/** Home's pinned rail item — same chrome as a mod entry, selected the same way. */
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

interface LoadAlertProps {
  errors: LoadError[];
  tr: Translator;
}

function LoadAlert({ errors, tr }: LoadAlertProps) {
  return (
    <div class="rail-alert">
      <div class="rail-alert-title">
        {tr.plural(
          'loadError',
          errors.length,
          '1 settings file failed to load',
          '{count} settings files failed to load',
        )}
      </div>
      {errors.map((e, i) => (
        <div key={`${e.file || e.mod || '?'}-${i}`} class="rail-alert-row">
          <div class="rail-alert-file">{e.file || e.mod || '?'}</div>
          <div class="rail-alert-msg">
            {e.kind === 'values-parse'
              ? // The one failure with a recovery story worth spelling out: the
                // mod still works, on defaults, and the unreadable file is kept.
                tr(
                  'loadErrorValues',
                  'Saved settings for {mod} were unreadable — defaults are in use; the old file is kept next to it as {file}.bad. ({message})',
                  { mod: e.mod || '?', file: e.file || '?', message: e.message || '' },
                )
              : e.message || tr('loadErrorGeneric', 'The file could not be loaded.')}
          </div>
        </div>
      ))}
    </div>
  );
}
