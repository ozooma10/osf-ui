// Rail.tsx — the left-hand list of things you can configure. The painted order
// comes from @lib/settings/rail's `railNodes` so this file and the LB/RB
// `cycleRail` walk cannot drift apart; the order itself is argued there.
//
// The modified-count badge is derived from the model on every render, so there
// is no row state to reconcile.

import { modifiedCount } from '@lib/settings/modified';
import { modIconSrc, type AssetRoots } from '@lib/settings/assets';
import {
  FRAMEWORK_ID,
  HOME_ID,
  railNodes,
  type ModRecord,
  type RailEntry,
  type ViewRecord,
} from '@lib/settings/rail';
import type { Translator } from '@lib/i18n';
import { initials, Mark } from './marks';

export interface RailProps {
  mods: ModRecord[];
  views: ViewRecord[];
  /** Pre-trimmed, pre-lowercased — railNodes does not normalise it. */
  query: string;
  selectedId: string | null;
  tr: Translator;
  /** Harness-only icon path overrides. Undefined in production. */
  assetRoots: AssetRoots | undefined;
  onSelect: (id: string) => void;
}

export function Rail(props: RailProps) {
  const { mods, views, query, selectedId, tr, assetRoots, onSelect } = props;

  const nodes = railNodes({ mods, views }, query);

  return (
    <nav id="rail-list" class="rail-list" aria-label="Installed mods">
      {nodes.map((node, i) => {
        switch (node.kind) {
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
                // Entry ids are unique across the rail (mods by id, view-only
                // entries behind a "view:" prefix), so each item's icon-failed
                // state stays with the right mod when the filter reorders.
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


/**
 * Sub-line text: "Framework" for OSF UI itself, the mod id for a settings mod,
 * a view-kind count for a view-only entry (whose id is synthetic, so not worth
 * showing).
 */
function railSub(entry: RailEntry, tr: Translator): string {
  if (entry.id === FRAMEWORK_ID) return tr('framework', 'Framework');
  if (entry.mod) return entry.mod.id;
  const menus = entry.views.filter((v) => v.kind === 'menu').length;
  const huds = entry.views.length - menus;
  const parts: string[] = [];
  // Compatibility catalog addresses retain the former terminal/overlay nouns.
  if (menus) parts.push(tr.plural('terminal', menus, 'Menu', '{count} menus'));
  if (huds) parts.push(tr.plural('overlay', huds, 'HUD', '{count} HUDs'));
  // A view-only entry is built from views, so zero views is unreachable; the
  // fallback is defence only.
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
