// Board.tsx — the visual keyboard map.
//
// Ports `renderKeyboard()` + `paintKeyboard()` (main.legacy.js:204-267). Legacy
// split them because it built the DOM once and then mutated classes on every
// search keystroke and every selection; a Preact render is the whole paint, so
// the two collapse into one component and the `keyCells` Map survives only for
// the hotkey flash (see below).

import { useLayoutEffect, useRef } from 'preact/hooks';
import { holdersOf, keyState } from '@lib/keybinds/conflicts';
import { isGap, KEYBOARD_MAIN, KEYBOARD_NAV, type LayoutItem } from '@lib/keybinds/layout';
import type { BindingRow } from '@lib/keybinds/model';
import type { Translator } from '@lib/i18n';
import { matchesQuery } from './search';

/**
 * The `ui.hotkey` flash. `seq` is a monotonic counter rather than a timestamp:
 * pressing the SAME hotkey twice must restart the animation, and only a change
 * in the dependency can trigger the restart effect below.
 */
export interface FlashState {
  name: string;
  /** 0 means "nothing has flashed yet" — no cell is animated at boot. */
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
   * False until the first render that had data. Legacy builds the board ONLY
   * from `renderAll()` (main.legacy.js:377), which in-game never runs before
   * `settings.data` arrives — so the shipped `#keyboard` div sits EMPTY until
   * then, and fills in (with the layout reflow that implies) on first data. The
   * sibling panels are gated the same way; the board was the odd one out.
   */
  loaded: boolean;
  onSelect: (name: string) => void;
}

export function Board(props: BoardProps) {
  const { bindings, query, selectedKey, flash, loaded, tr, onSelect } = props;

  // canonical key name -> cell node, exactly the role of legacy's `keyCells`
  // (main.legacy.js:202). The ONLY remaining consumer is the flash restart,
  // which cannot be expressed declaratively — see below.
  const cells = useRef(new Map<string, HTMLButtonElement>());

  // The flash is the one genuinely imperative bit of this view. Legacy does
  //   cell.classList.remove("is-flash"); void cell.offsetWidth; add("is-flash")
  // and the forced reflow is load-bearing: without it, re-adding a class the
  // element already has does not restart a CSS animation. Re-rendering with the
  // same class string is likewise a no-op to the diff, so the reflow dance has
  // to happen here on every `seq` change.
  //
  // The class ALSO appears in the computed class string below, so a re-render
  // that happens mid-animation does not tear it off again.
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
      // Dead cell: drawn, never bindable, and `disabled` so padnav's candidate
      // scan skips it (padnav.js:87). Esc gets its own reason string — it IS
      // resolvable, but the capture flow reads a press of it as "cancel".
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

    // Toggle order copied from main.legacy.js:251-256 so the emitted class
    // attribute matches the shipped one token for token.
    let className = 'kb-key';
    // A single holder gets the flat mod/game tint; two or more always fall
    // through to the shared/conflict styling below, which is why both tests
    // carry `holders.length === 1`.
    if (hasMod && holders.length === 1) className += ' is-mod';
    if (hasGame && !hasMod && holders.length === 1) className += ' is-game';
    // `shared && !conflict`: a key that is both (three holders, one expected
    // share plus a real collision) paints as the louder conflict.
    if (state.shared && !state.conflict) className += ' is-shared';
    if (state.conflict) className += ' is-conflict';
    if (name === selectedKey) className += ' is-selected';
    // Dimmed when a search is active and NEITHER any holder nor the key's own
    // name matches — so searching "f5" keeps F5 lit even if nothing is bound.
    if (query && !holders.some(matchesQuery(query)) && !name.toLowerCase().includes(query)) {
      className += ' is-dim';
    }
    if (flash.seq && flash.name === name) className += ' is-flash';

    // Tooltip is the holder list, one per line, falling back to the bare key
    // name when nothing holds it.
    const who = holders.map((b) => `${b.owner}: ${b.label}`).join('\n');

    return (
      <button
        key={`key:${name}`}
        type="button"
        class={className}
        // Legacy set `cell.dataset.name = item.n` on every live cell
        // (main.legacy.js:224). Nothing in-tree reads it (the cell lookup goes
        // through the `cells` ref, as legacy's went through `keyCells`), but it
        // is part of the shipped DOM shape, so it is preserved verbatim rather
        // than silently dropped.
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
              density hint, not a count. main.legacy.js:260. */}
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

  // The container is always present (it is static markup in legacy's
  // index.html), but its CONTENTS appear only once data has landed — see the
  // `loaded` doc above.
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
