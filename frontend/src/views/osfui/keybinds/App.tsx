// The Keybindings view: a keyboard map (mod-bound keys accent, game-bound steel,
// collisions warn), a holders panel for the selected key, and a searchable list.
//
// Mod key settings come from `osfui/settings`; the game's read-only binding
// catalog comes from `osfui/keybindings`. Rebinds reuse the generic capture
// machinery (`settings.captureKey` -> `settings.captured` -> echoed
// `settings.set`), including the capture-time conflict live-warn. `ui.hotkey`
// pushes flash the pressed key on the board.
//
// Grouping is by key name with the same alias folding as native
// (Tilde/Backtick/Console -> Grave, Return -> Enter), so the board agrees with
// the store's vk-resolved conflict data without re-resolving VKs in JS.
//
// No data-i18n here: `osfui.localize()` mutates text and attributes in place and
// caches the originals in element-keyed WeakMaps, so a Preact re-render reverts
// localised strings to the authored English and remounted nodes lose the cache
// entirely. Every string resolves through the @lib/i18n translator at render
// time instead. `osfui.localize` itself is untouched in the shared kit —
// third-party views still use it.

import { useEffect, useMemo, useRef, useState } from 'preact/hooks';
import { windowBridge, type Bridge } from '@lib/bridge';
import { makeTranslator } from '@lib/i18n';
import { codeOf } from '@lib/protocol';
import { canonicalName } from '@lib/keybinds/canonical';
import { matchesBindingFilter } from '@lib/keybinds/filter';

import { buildModel, type ModEntry } from '@lib/keybinds/model';
import type { BindingRow } from '@lib/keybinds/model';
import { makeLabeler } from '@lib/keybinds/labels';
import type { EngineInputContextState, KeybindingsData, SettingsData } from '@sdk';
import { BrandEmblem } from '@ui/BrandEmblem';
import { useLatest, useStateRef } from '@ui/useStateRef';
import { useKeyCapture, type KeyCapturePayload } from '@ui/useKeyCapture';
import { Scrim } from '@ui/Scrim';
import { SearchBox } from '@ui/SearchBox';
import { ToastStack, useToasts } from '@ui/Toast';
import { BindList } from './BindList';
import { Board, type FlashState } from './Board';
import { DetailPanel } from './DetailPanel';
import { matchesQuery } from './search';

/**
 * Back to Mod Settings rather than dismissing the overlay: single-menu policy
 * means opening Mod Settings replaces this menu, so no explicit close is needed.
 */
const MOD_SETTINGS_VIEW = 'osfui/settings';

/** The armed rebind. `instanceId` is the rendered row — see HolderRowProps. */
interface Capture {
  mod: string;
  key: string;
  instanceId: string;
}

export interface AppProps {
  /**
   * Defaulted because the dev harness mounts `<App />` with no props, so an
   * undefined bridge would be dereferenced by the translator before the first
   * render. Production passes it explicitly.
   */
  bridge?: Bridge;
}

export function App({ bridge = windowBridge }: AppProps) {
  const tr = useMemo(() => makeTranslator(bridge, 'chrome.keybinds'), [bridge]);

  // `mods` is mirrored into a ref because bridge subscriptions are registered
  // once and their closures would otherwise read the first render's values.
  const [mods, setMods, modsRef] = useStateRef<ModEntry[]>([]);
  const [liveKeys, setLiveKeys] = useState<KeybindingsData | null>(null);
  const [engineInputContext, setEngineInputContext] = useState<EngineInputContextState | null>(null);
  // Localized keycap labels for the player's layout (additive; undefined on
  // older OSF UI runtimes and in the preview — everything falls back to names/US glyphs).
  const [keyboard, setKeyboard] = useState<SettingsData['keyboard']>(undefined);

  const [selectedKey, setSelectedKey] = useState('');
  const [search, setSearch] = useState('');
  const [filter, setFilter] = useState('all');
  const [loaded, setLoaded] = useState(false);
  // Bumped on every `osfui/i18n` state update so the memo below re-runs: the model
  // carries translated strings, so a locale change has to rebuild it, not just
  // repaint.
  const [i18nSeq, setI18nSeq] = useState(0);
  const [flash, setFlash] = useState<FlashState>({ name: '', seq: 0 });

  const toasts = useToasts();
  // Same reason as mods: `push` is called from long-lived closures.
  const toastRef = useLatest(toasts);

  const searchRef = useRef<HTMLInputElement | null>(null);

  const labeler = useMemo(() => makeLabeler(keyboard), [keyboard]);
  const bindings = useMemo(
    () => buildModel(mods, liveKeys?.actions ?? [], tr, labeler),
    // eslint-disable-next-line react-hooks/exhaustive-deps -- i18nSeq is the locale generation.
    [mods, liveKeys, tr, labeler, i18nSeq],
  );
  const bindingsRef = useLatest(bindings);

  // Both consumers take this pre-normalised — see matchesQuery().
  const query = search.trim().toLowerCase();
  const shownBindingNames = useMemo<ReadonlySet<string> | null>(() => {
    // With no list scope, the board's normal mod/game/conflict language already
    // describes every occupied key. Search or a non-All filter turns the list
    // into a subset, so publish that exact subset to the map as a visual layer.
    if (!query && filter === 'all') return null;
    const queryMatches = matchesQuery(query);
    const names = new Set<string>();
    for (const binding of bindings) {
      if (
        binding.name
        && queryMatches(binding)
        && matchesBindingFilter(binding, filter, engineInputContext)
      ) {
        names.add(binding.name);
      }
    }
    return names;
  }, [bindings, query, filter, engineInputContext]);

  // With no bridge these are silent no-ops rather than rejected promises.
  const sendEndpoint = (endpoint: string, fields?: Record<string, unknown>) => {
    if (bridge.available()) bridge.send(endpoint, fields);
  };

  /**
   * Esc / pad-B and the header button. If the Mod Settings view was not discovered
   * (`unknown-view`) fall back to a plain close, so Esc can never strand the
   * user in a menu they cannot leave.
   */
  const goBack = () => {
    if (!bridge.available()) return;
    bridge
      .request('menu.open', { view: MOD_SETTINGS_VIEW })
      .catch(() => sendEndpoint('close'));
  };
  const capture = useKeyCapture<Capture>({
    bridge,
    requestFields: ({ mod, key }) => ({ mod, key }),
    onConflicts: (_target, name, conflicts) => {
      const others = [...new Set(conflicts.map((conflict) => conflict.title || conflict.mod))];
      toastRef.current.push(
        tr('alsoBoundBy', '{key} is also bound by: {others}', {
          // The localized keycap when known — the toast names what the player
          // just pressed, and their keycap says "Ö", not "Semicolon".
          key: labeler(canonicalName(name)) ?? name,
          others: others.join(', '),
        }),
        'warn',
      );
    },
    onCommit: ({ mod, key }, name) => {
      const next = modsRef.current.map((entry) =>
        entry && entry.id === mod
          ? { ...entry, values: { ...(entry.values || {}), [key]: name } }
          : entry,
      );
      setMods(next);

      if (bridge.available()) {
        bridge.request('settings.set', { mod, key, value: name }).catch((error: unknown) => {
          const code = codeOf(error);
          toastRef.current.push(
            tr('rebindRejected', 'Rebind rejected{code}', { code: code ? ` (${code})` : '' }),
            'danger',
          );
          // No re-read: the refused value never committed, and the store
          // republishes `osfui/settings` on any real change anyway.
        });
      }

      setSelectedKey(canonicalName(name));
      setLoaded(true);
    },
    onError: (error) => {
      toastRef.current.push(
        codeOf(error) === 'capture-busy'
          ? tr('captureBusy', 'Another rebind is already listening.')
          : tr('captureNoResponse', "Rebinding didn't get a response from the OSF UI runtime."),
        'warn',
      );
    },
  });

  const beginCapture = (binding: BindingRow, instanceId: string) => {
    if (binding.kind === 'mod') capture.begin({ mod: binding.mod, key: binding.key, instanceId });
  };
  /** Toggles: clicking the already-selected key clears the panel. */
  const selectKey = (name: string) => {
    setSelectedKey((current) => (name === current ? '' : name));
  };

  // Registered once. Each state handler runs immediately with the current
  // value, so there is no read to issue and nothing to re-issue after a reload.
  useEffect(() => {
    const offData = bridge.state('osfui/settings', (data) => {
      setMods(Array.isArray(data?.mods) ? data.mods : []);
      setKeyboard(data && typeof data.keyboard === 'object' ? data.keyboard : undefined);
      setLoaded(true);
    });

    const offLiveKeys = bridge.state('osfui/keybindings', (data) => {
      setLiveKeys(data && Array.isArray(data.actions) ? data : { available: false, revision: 0, gameVersion: '', actions: [] });
      setLoaded(true);
    });

    const offEngineInputContext = bridge.state('osfui/input-context', (data) => {
      setEngineInputContext(data || null);
    });

    const offI18n = bridge.state('osfui/i18n', () => {
      // A catalog that arrives before data must not hide the loading line.
      setI18nSeq((n) => n + 1);
    });

    const offChanged = bridge.on('settings.changed', (p) => {
      // Only key-typed settings matter here (the schema says which); other
      // traffic is ignored. The board derives collisions itself by key-name
      // grouping, so the pushed `conflicts` list needs no separate handling.
      const mod = modsRef.current.find((m) => m && m.id === p.mod);
      if (!mod) return;
      const isKey = ((mod.schema && mod.schema.groups) || []).some((g) =>
        ((g && g.settings) || []).some((s) => {
          const item = s as { key?: unknown; type?: unknown } | null;
          return !!item && item.key === p.key && item.type === 'key';
        }),
      );
      if (!isKey || typeof p.value !== 'string') return;
      const value = p.value;
      setMods(
        modsRef.current.map((m) =>
          m && m.id === p.mod ? { ...m, values: { ...(m.values || {}), [p.key]: value } } : m,
        ),
      );
      setLoaded(true);
    });

    // Belt-and-braces alongside the beginCapture promise: catches a reply that
    // lost its correlation (older OSF UI runtime without requestId echo). capture.finish
    // is idempotent.
    const offCaptured = bridge.on('settings.captured', (p) => capture.finish(p as KeyCapturePayload));

    const offHotkey = bridge.on('ui.hotkey', (p) => {
      const b = bindingsRef.current.find(
        (x) => x.kind === 'mod' && x.mod === p.mod && x.key === p.key,
      );
      // Nothing to flash for an unbound key; Board itself no-ops when no cell
      // carries the name.
      if (!b) return;
      setFlash((f) => ({ name: b.name, seq: f.seq + 1 }));
    });

    // The OSF UI runtime delegates the back action (Esc / pad-B) as a synthetic
    // Escape instead of closing the overlay, so the keydown handler below can
    // return to Mod Settings. Sticky per page load — re-asserted on every boot.
    // Asserted unconditionally rather than behind `ready`: the grant is
    // per-document and the OSF UI runtime drops it when this page reloads, so the
    // right moment to re-assert it is "whenever this component mounts".
    // Nothing here reads settings — the registry arrives as replayed state.
    if (bridge.available()) sendEndpoint('osfui.handleBack', { handle: true });

    return () => {
      offData();
      offLiveKeys();
      offEngineInputContext();
      offI18n();
      offChanged();
      offCaptured();
      offHotkey();
    };
    // eslint-disable-next-line react-hooks/exhaustive-deps -- registered once per bridge.
  }, [bridge]);

  useEffect(() => {
    const onKeyDown = (e: KeyboardEvent) => {
      if ((e.ctrlKey || e.metaKey) && String(e.key).toLowerCase() === 'f') {
        e.preventDefault();
        const input = searchRef.current;
        if (input) {
          input.focus();
          input.select();
        }
        return;
      }
      // `keyCode` is the fallback for synthetic key events where `e.key` is not
      // reliably "Escape". Swallowed while a capture is armed: the press belongs
      // to the rebind. (The standalone capture path also preventDefaults it in
      // the capture phase, which `defaultPrevented` catches independently.)
      if ((e.key === 'Escape' || e.keyCode === 27) && !e.defaultPrevented && !capture.isCapturing()) {
        goBack();
      }
    };
    document.addEventListener('keydown', onKeyDown);
    return () => document.removeEventListener('keydown', onKeyDown);
    // eslint-disable-next-line react-hooks/exhaustive-deps -- goBack only reads `bridge`.
  }, [bridge]);

  // Standalone preview: sample data so the view works in a plain browser. Dev-only,
  // and the production OSF UI runtime always injects a bridge, so this branch never ships.
  useEffect(() => {
    if (!import.meta.env.DEV) return;
    if (bridge.available()) return;
    // Cast: hand-written fixture, not a wire payload; spelling out every
    // optional field of SettingsSchema would obscure what it is testing.
    setMods([
      {
        id: 'osfui',
        title: 'OSF UI',
        values: { toggleKey: 'F10' },
        schema: {
          groups: [
            {
              settings: [
                { key: 'toggleKey', label: 'Open / close key', type: 'key', default: 'F10' },
              ],
            },
          ],
        },
      },
    ] as unknown as ModEntry[]);
    setLiveKeys({
      available: true,
      revision: 1,
      gameVersion: 'preview',
      actions: [
        {
          event: 'QuickSave', label: 'Quicksave', category: 'Gameplay',
          context: { id: 0, name: 'MainGameplay', order: 0 }, classification: 'core',
          modes: { definite: ['onFoot', 'ship', 'vehicle', 'zeroG'], possible: [] },
          sortIndex: 0, required: false,
          bindings: [{ slot: 'main', key: 'F5', chord: ['F5'], unbound: false }],
        },
      ],
    });
    setLoaded(true);
    // eslint-disable-next-line react-hooks/exhaustive-deps -- boot-time only.
  }, [bridge]);

  const capturingId = capture.capturing?.instanceId ?? null;

  return (
    <>
      <Scrim />

      <div class="keybinds">
        <header class="kb-head">
          <div class="brand">
            <BrandEmblem />
            <div class="brand-text">
              <div class="brand-line">
                <span class="wordmark-osf">OSF</span>
                <span class="wordmark-ui">UI</span>
              </div>
              {/* `inputMap` is a frozen translation address; the default copy uses the canonical name. */}
              <div class="osf-eyebrow brand-sub">{tr('inputMap', 'KEYBINDINGS')}</div>
            </div>
          </div>

          <SearchBox
            id="search"
            value={search}
            onInput={setSearch}
            placeholder={tr('searchPlaceholder', 'Find a key, action, or mod')}
            ariaLabel={tr('searchPlaceholder', 'Find a key, action, or mod')}
            kbd="Ctrl F"
            keyshortcuts="Control+F"
            inputClass="kb-search"
            inputRef={searchRef}
          />

          <button
            id="back"
            type="button"
            class="osf-btn osf-btn--ghost osf-btn--sm osf-close"
            aria-keyshortcuts="Escape"
            onClick={goBack}
          >
            {/* Compatibility catalog address; fallback copy uses Mod Settings. */}
            <span>{tr('backToMods', 'Back to Mod Settings')}</span>
            <kbd>Esc</kbd>
          </button>
        </header>

        <section class="kb-board-shell" aria-labelledby="keyboard-title">
          <div class="kb-board-head">
            <div>
              <div class="osf-eyebrow" id="keyboard-title">
                {tr('keyboardMap', 'Keyboard map')}
              </div>
              <p>{tr('instructions', 'Select a key to inspect every action assigned to it.')}</p>
            </div>
            {/* Hidden from assistive tech: every swatch is also encoded in the
                per-key tooltip. */}
            <div class="kb-legend osf-eyebrow" aria-hidden="true">
              <span class="legend-item">
                <i class="legend-swatch legend-mod" />
                <span>{tr('mod', 'Mod')}</span>
              </span>
              <span class="legend-item">
                <i class="legend-swatch legend-game" />
                <span>{tr('game', 'Game')}</span>
              </span>
              <span class="legend-item">
                <i class="legend-swatch legend-shared" />
                <span>{tr('shared', 'Shared')}</span>
              </span>
              <span class="legend-item">
                <i class="legend-swatch legend-conflict" />
                <span>{tr('conflict', 'Conflict')}</span>
              </span>
              <span class="legend-item">
                <i class="legend-swatch legend-possible" />
                <span>{tr('possible', 'Possible')}</span>
              </span>
              {shownBindingNames !== null ? (
                <span class="legend-item">
                  <i class="legend-swatch legend-prioritized" />
                  <span>{tr('inList', 'In list')}</span>
                </span>
              ) : null}
            </div>
          </div>
          <Board
            bindings={bindings}
            query={query}
            shownBindingNames={shownBindingNames}
            selectedKey={selectedKey}
            flash={flash}
            loaded={loaded}
            tr={tr}
            labeler={labeler}
            onSelect={selectKey}
          />
        </section>

        <div class="kb-lower">
          {/* No `query` prop — see the note in DetailPanel.tsx. */}
          <DetailPanel
            bindings={bindings}
            selectedKey={selectedKey}
            filter={filter}
            engineInputContext={engineInputContext}
            loaded={loaded}
            tr={tr}
            capturingId={capturingId}
            labeler={labeler}
            onRebind={beginCapture}
          />
          <BindList
            bindings={bindings}
            query={query}
            loaded={loaded}
            tr={tr}
            capturingId={capturingId}
            onRebind={beginCapture}
            onSelect={selectKey}
            filter={filter}
            onFilter={setFilter}
            engineInputContext={engineInputContext}
          />
        </div>

        {/* Absolute address: the shared catalog entry, outside this view's
            `chrome.keybinds` prefix. */}
        {loaded ? null : (
          <p id="status" class="kb-status osf-eyebrow">
            {tr('chrome.common.loading', 'Loading…')}
          </p>
        )}
      </div>

      <ToastStack id="toast" entries={toasts.entries} />
    </>
  );
}
