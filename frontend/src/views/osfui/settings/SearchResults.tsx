// SearchResults.tsx — the flat, cross-mod result list that REPLACES the detail
// pane while the filter box has text.
//
// Ports `renderSearch` (settings/main.legacy.js:1384-1426).
//
// It is a detail MODE, not an overlay: the first branch of the pane's dispatch
// (`if (q) { renderSearch(q); return; }`, main.legacy.js:1091), which is why
// typing while looking at a mod hides that mod's controls entirely rather than
// filtering them in place. The rail filters in parallel, so the two agree about
// which mods are involved.
//
// Clicking a result CLEARS THE FILTER and jumps: select the owning mod, expand
// the group the setting lives in (it may be collapsed), scroll it into view and
// flash it. That last part is why the scan keeps only settings with a real key
// — the jump target is `.row[data-key=…]`, and a keyless row has no anchor.
//
// `cssEscape` is not ported: legacy needed it to build that attribute selector
// by hand (main.legacy.js:1411, 1428-1431), and the port passes the key as
// data instead of interpolating it into a selector string.

import { searchResults, type SearchResult } from '@lib/settings/search';
import type { ModRecord } from '@lib/settings/rail';
import type { Translator } from '@lib/i18n';

export interface SearchResultsProps {
  mods: ModRecord[];
  /** Pre-trimmed, pre-lowercased. */
  query: string;
  tr: Translator;
  onJump: (result: SearchResult) => void;
}

export function SearchResults({ mods, query, tr, onJump }: SearchResultsProps) {
  const hits = searchResults(mods, query);

  return (
    <>
      <div class="detail-head">
        <div>
          <div class="osf-eyebrow kicker">{tr('search', 'Search')}</div>
          {/* The query is echoed inside literal quotes; it is user input, not
              schema text, and lands as a text child either way. */}
          <h2>{tr('resultsFor', 'Results for "{query}"', { query })}</h2>
        </div>
      </div>
      <div class="detail-body">
        <div class="search-results">
          {hits.length ? (
            hits.map((r) => (
              // A mod may declare the same key in two groups, so the group
              // label is part of the identity.
              <button
                key={`${r.modId} ${r.groupLabel} ${r.key}`}
                type="button"
                class="search-result"
                onClick={() => onJump(r)}
              >
                <div class="search-crumb">
                  <span class="search-mod">{r.modTitle}</span>
                  {r.groupLabel ? (
                    <>
                      <span class="search-sep">›</span>
                      <span>{r.groupLabel}</span>
                    </>
                  ) : null}
                </div>
                <div class="search-label">{r.label}</div>
              </button>
            ))
          ) : (
            <div class="detail-empty">{tr('noSettingsMatch', 'No settings match.')}</div>
          )}
        </div>
      </div>
    </>
  );
}
