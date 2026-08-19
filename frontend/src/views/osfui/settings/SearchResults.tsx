
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
          {/* User input, not schema text; lands as a text child either way. */}
          <h2>{tr('resultsFor', 'Results for "{query}"', { query })}</h2>
        </div>
      </div>
      <div class="detail-body">
        <div class="search-results">
          {hits.length ? (
            hits.map((r) => (
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
