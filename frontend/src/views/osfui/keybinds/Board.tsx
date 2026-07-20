// The visual keyboard map. A render is the whole paint; the `cells` Map exists
// only for the hotkey flash.

import { useLayoutEffect, useRef } from 'preact/hooks';
import { holdersOf, keyState } from '@lib/keybinds/conflicts';
import { isGap, KEYBOARD_MAIN, KEYBOARD_NAV, type LayoutItem } from '@lib/keybinds/layout';
import type { BindingRow } from '@lib/keybinds/model';
import type { Translator } from '@lib/i18n';
import { matchesQuery } from './search';

/**
 * The `ui.hotkey` flash. `seq` is a monotonic counter, not a timestamp: pressing
 * the same hotkey twice must restart the animation, and only a changed
 * dependency triggers the restart effect below.
 */
export interface FlashState {
  name: string;
  /** 0 means nothing has flashed yet — no cell is animated at boot. */
  seq: number;
}

export interface BoardProps {
  bindings: readonly BindingRow[];
  /** Already trimmed + lowercased by the caller, per matchesQuery(). */
  query: string;
  selectedKey: string;
  flash: FlashState;
  tr: Translator;
  /**
   * False until the first render that had data: the `#keyboard` div stays empty
   * until `settings.data` arrives, then fills in. Sibling panels gate the same way.
   */
  loaded: boolean;
  onSelect: (name: string) => void;
}

export function Board(props: BoardProps) {
  const { bindings, query, selectedKey, flash, loaded, tr, onSelect } = props;

  // Canonical key name -> cell node. Only consumer is the flash restart below,
  // which can't be expressed declaratively.
  const cells = useRef(new Map<string, HTMLButtonElement>());

  // The forced reflow is load-bearing: re-adding a class the element already has
  // does not restart a CSS animation, and re-rendering the same class string is a
  // no-op to the diff. So the dance runs here on every `seq` change.
  //
  // The class also appears in the computed class string below, so a re-render
  // mid-animation does not tear it off again.
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
      // Dead cell: drawn, not bindable, `disabled` so padnav's candidate scan
      // skips it. Esc gets its own reason string — it is resolvable, but the
      // capture flow reads a press of it as cancel.
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

    // Toggle order is fixed: the emitted class attribute must match the shipped
    // one token for token.
    let className = 'kb-key';
    // A single holder gets the flat mod/game tint; two or more fall through to
    // the shared/conflict styling below — hence `holders.length === 1` on both.
    if (hasMod && holders.length === 1) className += ' is-mod';
    if (hasGame && !hasMod && holders.length === 1) className += ' is-game';
    // `shared && !conflict`: a key that is both (three holders, one expected
    // share plus a real collision) paints as the louder conflict.
    if (state.shared && !state.conflict) className += ' is-shared';
    if (state.conflict) className += ' is-conflict';
    if (name === selectedKey) className += ' is-selected';
    // Dimmed when a search is active and neither any holder nor the key's own
    // name matches — searching "f5" keeps F5 lit even if nothing is bound.
    if (query && !holders.some(matchesQuery(query)) && !name.toLowerCase().includes(query)) {
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
        // Nothing in-tree reads this (cell lookup goes through the `cells` ref),
        // but it is part of the shipped DOM shape.
        data-name={name}
        style={{ flexGrow: item.w, flexBasis: 0 }}
        title={who || name}
        ref={(node) => {
          if (node) cells.current.set(name, node as HTMLButtonElement);
          else cells.current.delete(name);
        }}
        onClick={() => onSelect(name)}
      >
        <span class="kb-key-label">{item.d}</span>
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

  // The container is always present; its contents appear only once data lands.
  return (
    <div id="keyboard" class="kb-board" aria-label="Keyboard map">
      {loaded ? (
        <>
          {renderBlock(KEYBOARD_MAIN, 'main')}
          {renderBlock(KEYBOARD_NAV, 'nav')}
        </>
      ) : null}
    </div>
  );
}
