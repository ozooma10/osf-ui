// The keybinds view: a keyboard map (mod-bound keys accent, game-bound steel,
// collisions warn), a holders panel for the selected key, and a searchable list.
//
// Data is the same `settings.data` document the settings view consumes: every
// `type:"key"` setting of every mod, plus the top-level `vanillaKeys` table
// (the game's own bindings, read-only rows). Rebinds reuse the generic capture
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
import { canonicalName } from '@lib/keybinds/canonical';
import { domKeyName } from '@lib/keybinds/domKeyName';
import { buildModel, type ModEntry, type VanillaKey } from '@lib/keybinds/model';
import type { BindingRow } from '@lib/keybinds/model';
import { Scrim } from '@ui/Scrim';
import { SearchBox } from '@ui/SearchBox';
import { ToastStack, useToasts } from '@ui/Toast';
import { BindList } from './BindList';
import { Board, type FlashState } from './Board';
import { DetailPanel } from './DetailPanel';

/**
 * Back to the Mods hub rather than dismissing the overlay: single-menu policy
 * means opening the hub replaces this menu, so no explicit close is needed.
 */
const HUB_VIEW = 'osfui/settings';

/** The armed rebind. `instanceId` is the rendered row — see HolderRowProps. */
interface Capture {
  mod: string;
  key: string;
  instanceId: string;
}

/**
 * What `finishCapture` accepts: a real `settings.captured` payload, or the
 * synthetic cancel built locally on a rejected request. Every field is optional
 * because both shapes flow through one function.
 */
interface CapturePayload {
  name?: string;
  cancelled?: boolean;
  conflicts?: Array<{ mod?: string; title?: string }>;
}

function codeOf(err: unknown): string {
  const e = err as { code?: unknown } | null;
  return e && typeof e.code === 'string' ? e.code : '';
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

  // `mods`/`vanilla` are mirrored into refs because the bridge subscriptions are
  // registered once and their closures would otherwise read the first render's
  // values.
  const [mods, setModsState] = useState<ModEntry[]>([]);
  const [vanilla, setVanillaState] = useState<VanillaKey[]>([]);
  const modsRef = useRef<ModEntry[]>(mods);
  const vanillaRef = useRef<VanillaKey[]>(vanilla);
  const setMods = (next: ModEntry[]) => {
    modsRef.current = next;
    setModsState(next);
  };
  const setVanilla = (next: VanillaKey[]) => {
    vanillaRef.current = next;
    setVanillaState(next);
  };

  const [selectedKey, setSelectedKey] = useState('');
  const [search, setSearch] = useState('');
  const [loaded, setLoaded] = useState(false);
  // Bumped on every `i18n.data` push so the memo below re-runs: the model
  // carries translated strings, so a locale change has to rebuild it, not just
  // repaint.
  const [i18nSeq, setI18nSeq] = useState(0);
  const [flash, setFlash] = useState<FlashState>({ name: '', seq: 0 });
  const [capturing, setCapturingState] = useState<Capture | null>(null);
  const capturingRef = useRef<Capture | null>(null);
  const setCapturing = (next: Capture | null) => {
    capturingRef.current = next;
    setCapturingState(next);
  };

  const toasts = useToasts();
  // Same reason as mods/vanilla: `push` is called from long-lived closures.
  const toastRef = useRef(toasts);
  toastRef.current = toasts;

  const searchRef = useRef<HTMLInputElement | null>(null);

  const bindings = useMemo(
    () => buildModel(mods, vanilla, tr),
    // eslint-disable-next-line react-hooks/exhaustive-deps -- i18nSeq is the locale generation.
    [mods, vanilla, tr, i18nSeq],
  );
  const bindingsRef = useRef<BindingRow[]>(bindings);
  bindingsRef.current = bindings;

  // Both consumers take this pre-normalised — see matchesQuery().
  const query = search.trim().toLowerCase();

  // With no bridge these are silent no-ops rather than rejected promises.
  const sendCommand = (command: string, fields?: Record<string, unknown>) => {
    if (bridge.available()) bridge.send(command, fields);
  };

  /**
   * Esc / pad-B and the header button. If the hub view isn't registered
   * (`unknown-view`) fall back to a plain close, so Esc can never strand the
   * user in a menu they cannot leave.
   */
  const goBack = () => {
    if (!bridge.available()) return;
    bridge
      .request('menu.open', { view: HUB_VIEW })
      .catch(() => sendCommand('close'));
  };

  /**
   * Settle a capture. Idempotent: both the awaited request and the
   * belt-and-braces `settings.captured` subscription call this, and the second
   * delivery no-ops because `capturing` is already cleared.
   */
  const finishCapture = (payload: CapturePayload | null | undefined) => {
    const current = capturingRef.current;
    if (!current) return;
    const { mod, key } = current;
    setCapturing(null);
    if (!payload || payload.cancelled || !payload.name) return;
    const name = payload.name;

    // Live-warn (mcm-design §9): the runtime already checked the captured key
    // against every other binding — surface it now, before the commit lands.
    if (Array.isArray(payload.conflicts) && payload.conflicts.length) {
      const others = [...new Set(payload.conflicts.map((c) => c.title || c.mod))];
      toastRef.current.push(
        tr('alsoBoundBy', '{key} is also bound by: {others}', {
          key: name,
          others: others.join(', '),
        }),
        'warn',
      );
    }

    // Optimistic local apply plus the authoritative echo (settings.set). The
    // lookup uses the mod id we armed with, not `payload.mod`: a mis-correlated
    // reply then cannot write into a different mod's values.
    const next = modsRef.current.map((m) =>
      m && m.id === mod ? { ...m, values: { ...(m.values || {}), [key]: name } } : m,
    );
    setMods(next);

    if (bridge.available()) {
      // A refusal rejects the request: fall back to the store's truth instead
      // of keeping the optimistic value.
      bridge.request('settings.set', { mod, key, value: name }).catch((err: unknown) => {
        const code = codeOf(err);
        toastRef.current.push(
          tr('rebindRejected', 'Rebind rejected{code}', { code: code ? ` (${code})` : '' }),
          'danger',
        );
        sendCommand('settings.get');
      });
    }

    setSelectedKey(canonicalName(name));
    setLoaded(true);
  };

  const beginCapture = (binding: BindingRow, instanceId: string) => {
    if (capturingRef.current) return; // one at a time, view-wide
    const mod = binding.mod;
    if (!mod) return; // unreachable: only `kind:"mod"` rows render the button
    const key = binding.key;
    setCapturing({ mod, key, instanceId });

    if (bridge.available()) {
      // One awaited request for the whole rebind: the settings.captured reply
      // echoes this request's id even though the user may take seconds to press
      // a key (timeoutMs 0 — the reply itself settles it; Escape/refusal comes
      // back `cancelled`). A second arm anywhere rejects with "capture-busy".
      bridge
        .request('settings.captureKey', { mod, key }, { timeoutMs: 0 })
        .then((msg) => finishCapture(msg.payload as CapturePayload))
        .catch((err: unknown) => {
          // Only if our arm is still the live one: a rejection that arrives
          // after the capture settled some other way must not toast.
          const current = capturingRef.current;
          if (current && current.instanceId === instanceId) {
            finishCapture({ cancelled: true });
            toastRef.current.push(
              codeOf(err) === 'capture-busy'
                ? tr('captureBusy', 'Another rebind is already listening.')
                : tr('captureNoResponse', "Rebinding didn't get a response from the runtime."),
              'warn',
            );
          }
        });
      return;
    }

    // Standalone preview: no runtime to capture for us, so read the key from
    // the DOM. Capture phase + preventDefault, so the press never reaches the
    // document-level Escape handler below.
    const onKey = (e: KeyboardEvent) => {
      window.removeEventListener('keydown', onKey, true);
      e.preventDefault();
      const name = domKeyName(e);
      finishCapture({ name, cancelled: e.key === 'Escape' || !name });
    };
    window.addEventListener('keydown', onKey, true);
  };

  /** Toggles: clicking the already-selected key clears the panel. */
  const selectKey = (name: string) => {
    setSelectedKey((current) => (name === current ? '' : name));
  };

  // Registered once; replies that resolve a request() land here too.
  useEffect(() => {
    const offData = bridge.on('settings.data', (p) => {
      setMods(Array.isArray(p.mods) ? p.mods : []);
      setVanilla(Array.isArray(p.vanillaKeys) ? p.vanillaKeys : []);
      setLoaded(true);
    });

    const offI18n = bridge.on('i18n.data', () => {
      // A catalog that arrives before any data must not hide the loading line.
      if (modsRef.current.length || vanillaRef.current.length) {
        setI18nSeq((n) => n + 1);
        setLoaded(true);
      }
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
    // lost its correlation (older host without requestId echo). finishCapture
    // is idempotent.
    const offCaptured = bridge.on('settings.captured', (p) => finishCapture(p as CapturePayload));

    const offHotkey = bridge.on('ui.hotkey', (p) => {
      const b = bindingsRef.current.find(
        (x) => x.kind === 'mod' && x.mod === p.mod && x.key === p.key,
      );
      // Nothing to flash for an unbound key; Board itself no-ops when no cell
      // carries the name.
      if (!b) return;
      setFlash((f) => ({ name: b.name, seq: f.seq + 1 }));
    });

    // The runtime delegates the back action (Esc / pad-B) as a synthetic
    // Escape instead of closing the overlay, so the keydown handler below can
    // return to the Mods hub. Sticky per page load — re-asserted on every boot.
    void bridge.ready().then(() => {
      sendCommand('osfui.handleBack', { handle: true });
      sendCommand('settings.get');
    });

    // Sent again immediately: this early get serves a view that booted after
    // the runtime was already up, and the store treats a duplicate as
    // idempotent.
    if (bridge.available()) sendCommand('settings.get');

    return () => {
      offData();
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
      if ((e.key === 'Escape' || e.keyCode === 27) && !e.defaultPrevented && !capturingRef.current) {
        goBack();
      }
    };
    document.addEventListener('keydown', onKeyDown);
    return () => document.removeEventListener('keydown', onKeyDown);
    // eslint-disable-next-line react-hooks/exhaustive-deps -- goBack only reads `bridge`.
  }, [bridge]);

  // Standalone preview: sample data so the view works in a plain browser. Dev-only,
  // and the production host always injects a bridge, so this branch never ships.
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
    setVanilla([
      { event: 'QuickSave', title: 'Starfield (Quicksave)', name: 'F5' },
      { event: 'QuickLoad', title: 'Starfield (Quickload)', name: 'F9' },
      { event: 'Activate', title: 'Starfield (Interact)', name: 'E' },
      { event: 'Jump', title: 'Starfield (Jump)', name: 'Space' },
      { event: 'Console', title: 'Starfield (Console)', name: 'Grave' },
    ]);
    setLoaded(true);
    // eslint-disable-next-line react-hooks/exhaustive-deps -- boot-time only.
  }, [bridge]);

  const capturingId = capturing ? capturing.instanceId : null;

  return (
    <>
      <Scrim />

      <div class="keybinds">
        <header class="kb-head">
          <div class="brand">
            <svg class="brand-emblem" width="34" height="34" viewBox="0 0 200 200" aria-hidden="true">
              <circle cx="100" cy="100" r="92" fill="#11151b" stroke="#5c646e" stroke-width="3" />
              <circle cx="100" cy="100" r="82" fill="none" stroke="#2f353d" stroke-width="1.2" />
              <g fill="none" stroke="#9fc7dc" stroke-linecap="round" stroke-width="8">
                <line x1="62" y1="74" x2="138" y2="74" />
                <line x1="62" y1="100" x2="138" y2="100" />
                <line x1="62" y1="126" x2="138" y2="126" />
              </g>
              <circle cx="84" cy="74" r="9" fill="#0b0e12" stroke="#9fc7dc" stroke-width="6" />
              <circle cx="120" cy="100" r="9" fill="#0b0e12" stroke="#9fc7dc" stroke-width="6" />
              <circle cx="92" cy="126" r="9" fill="#0b0e12" stroke="#9fc7dc" stroke-width="6" />
            </svg>
            <div class="brand-text">
              <div class="brand-line">
                <span class="wordmark-osf">OSF</span>
                <span class="wordmark-ui">UI</span>
              </div>
              <div class="osf-eyebrow brand-sub">{tr('inputMap', 'INPUT MAP')}</div>
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
            <span>{tr('backToMods', 'Back to Mods')}</span>
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
            </div>
          </div>
          <Board
            bindings={bindings}
            query={query}
            selectedKey={selectedKey}
            flash={flash}
            loaded={loaded}
            tr={tr}
            onSelect={selectKey}
          />
        </section>

        <div class="kb-lower">
          {/* No `query` prop — see the note in DetailPanel.tsx. */}
          <DetailPanel
            bindings={bindings}
            selectedKey={selectedKey}
            loaded={loaded}
            tr={tr}
            capturingId={capturingId}
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
