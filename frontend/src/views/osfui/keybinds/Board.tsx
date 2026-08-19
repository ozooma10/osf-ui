
import { useLayoutEffect, useRef } from 'preact/hooks';
import { holdersOf, keyState } from '@lib/keybinds/conflicts';
import { isGap, KEYBOARD_NAV, mainBlock, type LayoutItem } from '@lib/keybinds/layout';
import type { KeyLabeler } from '@lib/keybinds/labels';
import type { BindingRow } from '@lib/keybinds/model';
import type { Translator } from '@lib/i18n';
import { matchesQuery } from './search';

export interface FlashState {
  name: string;
  /** 0 means nothing has flashed yet — no cell is animated at boot. */
  seq: number;
}

export interface BoardProps {
  bindings: readonly BindingRow[];
  /** Already trimmed + lowercased by the caller, per matchesQuery(). */
  query: string;
  shownBindingNames?: ReadonlySet<string> | null;
  selectedKey: string;
  flash: FlashState;
  tr: Translator;
  loaded: boolean;
  labeler?: KeyLabeler;
  onSelect: (name: string) => void;
}

export function Board(props: BoardProps) {
  const {
    bindings,
    query,
    shownBindingNames = null,
    selectedKey,
    flash,
    loaded,
    tr,
    onSelect,
  } = props;
  const labeler: KeyLabeler = props.labeler ?? (() => undefined);

  const cells = useRef(new Map<string, HTMLButtonElement>());

  useLayoutEffect(() => {
    if (!flash.seq) return;
    const cell = cells.current.get(flash.name);
    if (!cell) return;
    cell.classList.remove('is-flash');
    void cell.offsetWidth; // force reflow — restarts the animation
    cell.classList.add('is-flash');
  }, [flash.seq, flash.name]);

  const renderItem = (item: LayoutItem, index: number) => {
    if (isGap(item)) {
      return <span key={`gap${index}`} class="kb-gap" style={{ flexGrow: item.gap }} />;
    }

    const name = item.n;
    if (!name) {
      return (
        <button
          key={`dead${index}:${item.d}`}
          type="button"
          class="kb-key is-dead"
          disabled
          style={{ flexGrow: item.w, flexBasis: 0 }}
          title={
            item.d === 'Esc'
              ? tr('reservedKey', 'Reserved (cancels rebinds)')
              : tr('notBindable', 'Not bindable by mods')
          }
        >
          <span class="kb-key-label">{item.d}</span>
        </button>
      );
    }

    const holders = holdersOf(bindings, name);
    const hasMod = holders.some((b) => b.kind === 'mod');
    const hasGame = holders.some((b) => b.kind === 'game');
    const state = keyState(bindings, name);
    const face = labeler(name) ?? item.d;

    let className = 'kb-key';
    if (hasMod && holders.length === 1) className += ' is-mod';
    if (hasGame && !hasMod && holders.length === 1) className += ' is-game';
    if (state.shared && !state.conflict) className += ' is-shared';
    if (state.possible && !state.conflict) className += ' is-possible';
    if (state.conflict) className += ' is-conflict';
    const bareKeyMatchesQuery = !!query && (
      name.toLowerCase().includes(query) || face.toLowerCase().includes(query)
    );
    if (shownBindingNames !== null) {
      if (shownBindingNames.has(name)) className += ' is-prioritized';
      else if (!bareKeyMatchesQuery) className += ' is-dim';
    }
    if (name === selectedKey) className += ' is-selected';
    if (shownBindingNames === null &&
      query &&
      !holders.some(matchesQuery(query)) &&
      !bareKeyMatchesQuery
    ) {
      className += ' is-dim';
    }
    if (flash.seq && flash.name === name) className += ' is-flash';

    // Tooltip is the holder list, one per line; bare key name when unheld.
    const who = holders.map((b) => `${b.owner}: ${b.label}`).join('\n');

    return (
      <button
        key={`key:${name}`}
        type="button"
        class={className}
        data-name={name}
        style={{ flexGrow: item.w, flexBasis: 0 }}
        title={who || face}
        ref={(node) => {
          if (node) cells.current.set(name, node as HTMLButtonElement);
          else cells.current.delete(name);
        }}
        onClick={() => onSelect(name)}
      >
        <span class="kb-key-label">{face}</span>
        <span class="kb-key-holders">
          {/* Capped at three: the cell is 36px tall and the dots are a
              density hint, not a count. */}
          {holders.slice(0, 3).map((b, i) => (
            <i key={i} class={`kb-dot kb-dot--${b.kind}`} />
          ))}
        </span>
      </button>
    );
  };

  const renderBlock = (rows: readonly (readonly LayoutItem[])[], blockKey: string) => (
    <div key={blockKey} class="kb-block">
      {rows.map((row, r) => (
        <div key={`${blockKey}${r}`} class="kb-row">
          {row.map(renderItem)}
        </div>
      ))}
    </div>
  );

  return (
    <div id="keyboard" class="kb-board" aria-label="Keyboard map">
      {loaded ? (
        <>
          {renderBlock(mainBlock(labeler('IntlBackslash') !== undefined), 'main')}
          {renderBlock(KEYBOARD_NAV, 'nav')}
        </>
      ) : null}
    </div>
  );
}
