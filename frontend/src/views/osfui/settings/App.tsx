// The Mods surface: two-pane master/detail, and the overlay's front door (the
// toggle key opens this view directly).
//
// The rail is topped by Home, a card grid of every registered panel and overlay
// across all mods. Below it, every installed mod — OSF UI pinned first, then the
// union of settings schemas (`settings.data`) and catalog views (`views.data`).
// The right pane renders the selected mod's surfaces, then its typed settings
// controls on the shared kit.
//
// Everything the schema adds beyond bool/int/float/enum/flags/string/key is
// presentation: widget hints, number formatting, visibleWhen/enabledWhen, note
// and image blocks, action buttons, requires badges, presets, the rail icon. The
// native SettingsStore trusts none of it — a hidden or disabled control is still
// validated on write, and an action command is refused unless it is namespaced
// to the owning mod. Untrusted schema text only reaches the DOM as a text child.
//
// No data-i18n in this view: `osfui.localize()` mutates text in place and caches
// originals in element-keyed WeakMaps, which a Preact re-render reverts and a
// remount forgets. Strings go through the @lib/i18n translator at render time.
// `osfui.localize` stays in the shared kit for third-party views.

import { useEffect, useMemo, useRef, useState } from 'preact/hooks';
import { windowBridge, type Bridge } from '@lib/bridge';
import { makeTranslator } from '@lib/i18n';
import { Scrim } from '@ui/Scrim';
import { SearchBox } from '@ui/SearchBox';
import { ToastStack, useToasts } from '@ui/Toast';
import { ACTION_TIMEOUT_MS } from '@ui/ActionButton';
import type { AssetRoots } from '@lib/settings/assets';
import {
  HOME_ID,
  railNodes,
  titleOf,
  type LoadError,
  type ModRecord,
  type ViewRecord,
} from '@lib/settings/rail';
import { applyConflictUpdate } from '@lib/settings/conflicts';
import { findSettingInMod, sameValue, sessionDiff, type Baseline } from '@lib/settings/modified';
import { deriveNeedsUpdate } from '@lib/version';
import {
  initialPadButtonState,
  reduceGamepad,
  reduceVisibility,
  type PadButtonState,
} from '@lib/lifecycle';
import {
  initialSaveState,
  saveStateAbandon,
  saveStatePending,
  saveStatePersisted,
  type SaveState,
  type SaveTransition,
} from '@lib/saveState';
import type { SettingValue } from '@sdk';
import { Detail } from './Detail';
import { Rail } from './Rail';
import { UndoPanel } from './UndoPanel';
import { homeModCaption } from './Home';
import { useCapture } from './useCapture';
import type { PresetRecord } from './Presets';

/**
 * Filter debounce. Every keystroke would otherwise re-scan every mod's schema
 * for the cross-mod result list and rebuild the rail.
 */
const FILTER_DEBOUNCE_MS = 120;
/** How long a search-jump target stays highlighted. */
const FLASH_MS = 1200;
/** Where the "needs update" tag sends the user to fetch a newer OSF UI. */
const NEXUS_PAGE_URL = 'https://www.nexusmods.com/starfield/mods/17711';

function codeOf(err: unknown): string {
  const e = err as { code?: unknown } | null;
  return e && typeof e.code === 'string' ? e.code : '';
}

export interface AppProps {
  /**
   * Defaults to the real bridge so the dev harness — which mounts `<App />`
   * with no props — gets the mock-decorated `window.osfui` it installed.
   */
  bridge?: Bridge;
  /**
   * Mod-id -> asset-root overrides for schema `icon` / `image` paths. The dev
   * harness serves this page from a directory where the shipped "../../<modId>"
   * assumption is false and passes its map here. Production passes nothing, so
   * the shipped path cannot be redirected by anything that sets a global — the
   * reason this is a prop and not a window lookup.
   */
  assetRoots?: AssetRoots;
}

export function App({ bridge = windowBridge, assetRoots }: AppProps) {
  const tr = useMemo(() => makeTranslator(bridge, 'chrome.settings'), [bridge]);

  // Every piece of long-lived state is mirrored into a ref: the bridge
  // subscriptions are registered once and their closures would otherwise read
  // the first render's values.

  const [mods, setModsState] = useState<ModRecord[]>([]);
  const modsRef = useRef<ModRecord[]>(mods);
  const setMods = (next: ModRecord[]) => {
    modsRef.current = next;
    setModsState(next);
  };

  const [views, setViewsState] = useState<ViewRecord[]>([]);
  const viewsRef = useRef<ViewRecord[]>(views);
  const setViews = (next: ViewRecord[]) => {
    viewsRef.current = next;
    setViewsState(next);
  };

  const [loadErrors, setLoadErrors] = useState<LoadError[]>([]);
  // Unfiltered catalog, including hub:false utility surfaces. The launcher
  // still honors hub:false, but framework diagnostics must be able to target
  // every loaded view (including this Mods surface itself).
  const [allViews, setAllViews] = useState<ViewRecord[]>([]);
  const [hostVersion, setHostVersion] = useState('');

  const [selectedId, setSelectedIdState] = useState<string | null>(null);
  const selectedIdRef = useRef<string | null>(null);
  const setSelectedId = (next: string | null) => {
    selectedIdRef.current = next;
    setSelectedIdState(next);
  };

  // `filter` is what the input shows (immediate); `query` is what the rail and
  // the detail pane consume (debounced, trimmed, lowercased — both consumers
  // take it pre-normalised).
  const [filter, setFilterState] = useState('');
  const filterRef = useRef('');
  const setFilter = (next: string) => {
    filterRef.current = next;
    setFilterState(next);
  };
  const [query, setQuery] = useState('');
  const queryRef = useRef('');
  queryRef.current = query;

  /**
   * `baseline[modId][key]` — the value when this visit began. Drives the undo
   * chip and the revert panel. Kept across data refreshes (so a reset or preset
   * re-broadcast does not lose undo history) and cleared on every overlay open
   * edge, so the scope is "since you opened settings", not the whole session.
   */
  const [baseline, setBaselineState] = useState<Baseline>({});
  const baselineRef = useRef<Baseline>({});
  const setBaseline = (next: Baseline) => {
    baselineRef.current = next;
    setBaselineState(next);
  };

  const [collapsed, setCollapsed] = useState<Record<string, boolean>>({});
  /**
   * Optimistic HUD switch positions, keyed by view id. `hud.show`/`hud.hide`
   * are fire-and-forget, so the switch flips locally and the next `views.data`
   * push (which the runtime sends on every open/focus change) is authoritative
   * — that push clears the whole map.
   */
  const [hudOverride, setHudOverride] = useState<Record<string, boolean>>({});
  const [undoOpen, setUndoOpen] = useState(false);
  const [flash, setFlash] = useState<{ modId: string; key: string } | null>(null);
  const [i18nSeq, setI18nSeq] = useState(0);

  const [save, setSaveState] = useState<SaveState>(initialSaveState);
  const saveRef = useRef<SaveState>(initialSaveState);
  const fadeTimer = useRef<ReturnType<typeof setTimeout> | null>(null);
  const applySave = (t: SaveTransition) => {
    saveRef.current = t.state;
    setSaveState(t.state);
    if (t.cancelFade && fadeTimer.current !== null) {
      clearTimeout(fadeTimer.current);
      fadeTimer.current = null;
    }
    if (t.scheduleFadeMs !== null) {
      fadeTimer.current = setTimeout(() => {
        saveRef.current = { ...saveRef.current, classes: [] };
        setSaveState(saveRef.current);
      }, t.scheduleFadeMs);
    }
  };

  const padRef = useRef<PadButtonState>(initialPadButtonState);

  /**
   * `#mod=<entry id>` preselects a rail entry (harness deep links, headless
   * screenshots). In game the view loads without a fragment, so it is inert.
   * Held pending until the entry exists: settings.data and views.data arrive in
   * either order, and the default-selection fallback must not eat it.
   */
  const pendingHashSelect = useRef<string | null>(null);
  const hashRead = useRef(false);
  if (!hashRead.current) {
    hashRead.current = true;
    const m = /^#mod=(.+)$/.exec((typeof location !== 'undefined' && location.hash) || '');
    if (m && m[1]) {
      try {
        pendingHashSelect.current = decodeURIComponent(m[1]);
      } catch {
        // Malformed escape in a hand-typed fragment: ignore rather than taking
        // the view down before the first paint.
        pendingHashSelect.current = null;
      }
    }
  }

  const toasts = useToasts();
  const toastRef = useRef(toasts);
  toastRef.current = toasts;
  const toast = (message: string, kind?: 'warn' | 'danger') => {
    // `exactOptionalPropertyTypes` forbids passing an explicit undefined where
    // the absence is what suppresses the modifier.
    if (kind === undefined) toastRef.current.push(message);
    else toastRef.current.push(message, kind);
  };

  const filterInput = useRef<HTMLInputElement | null>(null);

  const sendCommand = (command: string, fields?: Record<string, unknown>) => {
    if (bridge.available()) bridge.send(command, fields);
  };

  /**
   * Push one value. The availability check must come before `saveStatePending`:
   * marking pending with no bridge leaves the standalone harness at "Saving…"
   * forever, with nothing that can clear it.
   */
  const setValue = (modId: string, key: string, value: SettingValue) => {
    if (!bridge.available()) return;
    applySave(saveStatePending(saveRef.current, modId));
    // The ack resolves with the authoritative post-clamp value; a refusal
    // rejects with the machine code (unknown-setting / read-only /
    // invalid-value).
    bridge.request('settings.set', { mod: modId, key, value }).catch((err: unknown) => {
      const code = codeOf(err);
      toast(
        tr('writeRejected', 'Rejected {setting}{code}', {
          setting: `"${modId}.${key}"`,
          code: code ? ` (${code})` : '',
        }),
        'danger',
      );
      applySave(saveStateAbandon(saveRef.current, modId));
      // Native refused the value; pull authoritative state back.
      sendCommand('settings.get');
    });
  };

  /** Reset one key, or the whole mod when `key` is null. Same ordering rule. */
  const requestReset = (modId: string, key: string | null) => {
    if (!bridge.available()) return;
    applySave(saveStatePending(saveRef.current, modId));
    // Resolves with the fresh settings.data (rendered by the subscription —
    // request replies dispatch there too).
    bridge
      .request('settings.reset', key ? { mod: modId, key } : { mod: modId })
      .catch((err: unknown) => {
        const code = codeOf(err);
        toast(tr('resetFailed', 'Reset failed{code}', { code: code ? ` (${code})` : '' }), 'danger');
        applySave(saveStateAbandon(saveRef.current, modId));
      });
  };

  /**
   * Apply values optimistically so conditions and modified dots update on the
   * same frame as the click, and record the pre-change value against the
   * session baseline. Batched over several keys because a preset commits many
   * at once and one state update per key would render the pane N times.
   */
  const applyLocal = (modId: string, entries: Array<[string, SettingValue]>) => {
    const mod = modsRef.current.find((m) => m.id === modId);
    if (!mod) return;
    const values = mod.values || {};

    const nextBaseline: Baseline = { ...baselineRef.current };
    const tracked = { ...(nextBaseline[modId] || {}) };
    let baselineChanged = false;
    for (const [key] of entries) {
      // Seeded once per key: the first change is what the visit is measured
      // from, and a second edit of the same key must not move the goalposts.
      if (!(key in tracked)) {
        tracked[key] = values[key];
        baselineChanged = true;
      }
    }
    if (baselineChanged) {
      nextBaseline[modId] = tracked;
      setBaseline(nextBaseline);
    }

    const patch: Record<string, SettingValue> = {};
    for (const [key, value] of entries) patch[key] = value;
    setMods(
      modsRef.current.map((m) =>
        m.id === modId ? { ...m, values: { ...(m.values || {}), ...patch } } : m,
      ),
    );
  };

  /** A user commit: local model first, then the wire. */
  const commit = (modId: string, key: string, value: SettingValue) => {
    applyLocal(modId, [[key, value]]);
    setValue(modId, key, value);
  };

  /**
   * Seed the baseline for every key of every mod that lacks one, on each data
   * arrival — after a visibility reset that amounts to a full snapshot at the
   * first arrival. Lazy per-key seeding on first change would instead make the
   * undo list report only keys touched through this pane, missing external
   * writers.
   */
  const captureBaseline = (list: ModRecord[]) => {
    const next: Baseline = { ...baselineRef.current };
    let changed = false;
    for (const mod of list) {
      const tracked = { ...(next[mod.id] || {}) };
      let modChanged = false;
      for (const key in mod.values || {}) {
        if (!(key in tracked)) {
          tracked[key] = (mod.values || {})[key];
          modChanged = true;
        }
      }
      if (modChanged || !next[mod.id]) {
        next[mod.id] = tracked;
        changed = true;
      }
    }
    if (changed) setBaseline(next);
  };

  const selectMod = (id: string) => setSelectedId(id);

  /**
   * Default selection, re-run whenever the entry set changes: settings.data and
   * views.data arrive in either order, and a mod that unregisters mid-visit must
   * not leave the pane pointed at nothing.
   */
  useEffect(() => {
    const nodes = railNodes({ mods, views, loadErrors }, '');
    const ids = nodes.filter((n) => n.kind === 'entry').map((n) => (n as { entry: { id: string } }).entry.id);
    if (!ids.length) return; // nothing registered

    const pending = pendingHashSelect.current;
    if (pending && (pending === HOME_ID || ids.includes(pending))) {
      pendingHashSelect.current = null; // honoured once; later pushes must not override clicks
      setSelectedId(pending);
      return;
    }
    const current = selectedIdRef.current;
    if (current !== HOME_ID && (current === null || !ids.includes(current))) {
      setSelectedId(HOME_ID); // the launcher is the deck's landing page
    }
  }, [mods, views, loadErrors]);

  useEffect(() => {
    const t = setTimeout(() => setQuery(filter.trim().toLowerCase()), FILTER_DEBOUNCE_MS);
    return () => clearTimeout(t);
  }, [filter]);

  const capture = useCapture({
    bridge,
    modsRef,
    onCommit: commit,
    toast,
    tr,
  });

  // Bridge subscriptions, registered once. Replies that resolve a request()
  // also land here — one render path regardless of who asked.

  useEffect(() => {
    const offSettings = bridge.on('settings.data', (p) => {
      const list = (p.mods || []) as ModRecord[];
      setMods(list);
      setLoadErrors(Array.isArray(p.loadErrors) ? p.loadErrors : []);
      captureBaseline(list);
    });

    const offViews = bridge.on('views.data', (p) => {
      const all = (p.views || []) as ViewRecord[];
      // Version targets come off the unfiltered catalog — a `hub:false` utility
      // view still gets to ask for a newer host.
      setAllViews(all.filter((v) => v));
      setViews(all.filter((v) => v && v.hub !== false));
      // This push is the authority on HUD open state; drop the optimistic
      // overrides.
      setHudOverride({});
    });

    const offI18n = bridge.on('i18n.data', () => {
      // A catalog arriving before any data must not force a paint of an empty
      // surface.
      if (modsRef.current.length || viewsRef.current.length) setI18nSeq((n) => n + 1);
    });

    const offChanged = bridge.on('settings.changed', (p) => {
      // Native push for every committed value — our own commits echo back
      // (possibly clamped), and other writers (a sibling DLL, a mod's panel, a
      // preset applied in another view) stay in sync while the menu is open.
      if (typeof p.mod !== 'string' || typeof p.key !== 'string') return;
      const modId = p.mod;
      const key = p.key;
      const mod = modsRef.current.find((m) => m.id === modId);
      if (!mod) return;

      // Seed the baseline before overwriting, so an external writer's change is
      // undoable too.
      const nextBaseline: Baseline = { ...baselineRef.current };
      const tracked = { ...(nextBaseline[modId] || {}) };
      if (!(key in tracked)) {
        tracked[key] = (mod.values || {})[key];
        nextBaseline[modId] = tracked;
        setBaseline(nextBaseline);
      }

      const changedSetting = findSettingInMod(mod, key);
      if (changedSetting && changedSetting.type === 'key') {
        // Key-typed pushes carry the setting's recomputed `conflicts`
        // (protocol 0.5): apply both sides of the collision to the local model
        // instead of re-fetching the whole registry. Handled before the echo
        // check so our own rebind — already applied optimistically — still
        // updates the badges.
        const withValue = modsRef.current.map((m) =>
          m.id === modId ? { ...m, values: { ...(m.values || {}), [key]: p.value as SettingValue } } : m,
        );
        setMods(
          applyConflictUpdate(withValue, modId, key, Array.isArray(p.conflicts) ? p.conflicts : []),
        );
        return;
      }

      if (sameValue((mod.values || {})[key], p.value)) {
        // Echo of our own optimistic commit; the derived chip and rail counts
        // already reflect it.
        return;
      }
      // The store disagrees with the local model (a native clamp, or an
      // external writer): adopt its value.
      setMods(
        modsRef.current.map((m) =>
          m.id === modId ? { ...m, values: { ...(m.values || {}), [key]: p.value as SettingValue } } : m,
        ),
      );
    });

    // Backstop alongside the useCapture promise: catches a reply that lost its
    // correlation (an older host that does not echo requestId). `finish` is
    // idempotent — a second delivery no-ops.
    const offCaptured = bridge.on('settings.captured', (p) => capture.finish(p as never));

    const offPersisted = bridge.on('settings.persisted', (p) => {
      // The mod's values file write landed (write-behind flush) — distinct from
      // settings.changed, which is the immediate in-memory commit.
      applySave(saveStatePersisted(saveRef.current, p.mod));
    });

    const offVisibility = bridge.on('ui.visibility', (p) => {
      const intent = reduceVisibility(
        { selectedId: selectedIdRef.current ?? '', filter: filterRef.current },
        p,
      );
      if (!intent.clearBaseline) return; // hide edge
      // Fresh visit: the undo scope is "since you opened settings". Without
      // this the view — which keeps running while hidden — accumulates every
      // change of the whole game session.
      setBaseline({});
      if (intent.reselect) {
        // "Open the deck", not "resume where a past visit left off".
        setFilter('');
        setQuery('');
        setSelectedId(intent.state.selectedId);
      }
      // Outside the reselect guard: a visit that lands back on an
      // already-selected Home still forgets the gamepad resume point.
      const padnav = (window as { padnav?: { reset?: () => void } }).padnav;
      if (padnav && padnav.reset) padnav.reset();
    });

    // LB / RB step the rail selection. Raw `ui.gamepad` events ride alongside
    // the runtime's default mapping: we do not assert `osfui.gamepadRaw`, so
    // D-pad/A/B keep their native arrows/Enter/close behaviour. On hosts
    // without gamepad routing these never arrive.
    const offGamepad = bridge.on('ui.gamepad', (p) => {
      const nodes = railNodes(
        { mods: modsRef.current, views: viewsRef.current },
        queryRef.current,
      );
      const railIds = nodes
        .filter((n) => n.kind === 'home' || n.kind === 'entry')
        .map((n) => (n.kind === 'home' ? HOME_ID : (n as { entry: { id: string } }).entry.id));
      const intent = reduceGamepad(padRef.current, p, {
        railIds,
        selectedId: selectedIdRef.current ?? '',
        modalOpen: undoOpenRef.current,
      });
      padRef.current = intent.state;
      if (intent.select !== null) selectMod(intent.select);
    });

    // The runtime delegates the back action (Esc / pad-B) as a synthetic
    // Escape instead of closing the overlay, so the keydown handler below can
    // peel the undo panel first. Sticky per page load.
    if (bridge.available()) sendCommand('osfui.handleBack', { handle: true });

    // The initial reads must not be gated on `ready`. `runtime.ready` is a
    // one-shot greeting emitted at runtime init, which can be long before this
    // page's transport can carry it (the WebView2 host is a separate process
    // that starts on the first game tick); gating on it left the Mods surface
    // permanently empty whenever the greeting was missed. The gets are
    // idempotent and also subscribe to the change pushes.
    if (bridge.available()) {
      sendCommand('settings.get');
      sendCommand('views.get');
    }

    // The version badge is the only consumer of the handshake; it stays blank
    // until (and if) it lands.
    void bridge.ready().then((info) => {
      setHostVersion(info.version || '');
    });

    return () => {
      offSettings();
      offViews();
      offI18n();
      offChanged();
      offCaptured();
      offPersisted();
      offVisibility();
      offGamepad();
    };
    // eslint-disable-next-line react-hooks/exhaustive-deps -- registered once
    // per bridge.
  }, [bridge]);

  // `undoOpen` is read from the gamepad subscription's stale closure.
  const undoOpenRef = useRef(undoOpen);
  undoOpenRef.current = undoOpen;

  useEffect(() => {
    const onKeyDown = (e: KeyboardEvent) => {
      if ((e.ctrlKey || e.metaKey) && String(e.key).toLowerCase() === 'f') {
        e.preventDefault();
        const input = filterInput.current;
        if (input) {
          input.focus();
          input.select();
        }
        return;
      }
      // `keyCode` is a fallback for synthetic key events where `e.key` is not
      // reliably "Escape". Swallowed while a capture is armed — the press
      // belongs to the rebind.
      if ((e.key === 'Escape' || e.keyCode === 27) && !e.defaultPrevented && !capture.isCapturing()) {
        // Peel the undo panel first; only a bare Escape closes the surface.
        if (undoOpenRef.current) {
          setUndoOpen(false);
          return;
        }
        sendCommand('close');
      }
    };
    document.addEventListener('keydown', onKeyDown);
    return () => document.removeEventListener('keydown', onKeyDown);
    // eslint-disable-next-line react-hooks/exhaustive-deps -- reads only refs
    // and `bridge`.
  }, [bridge]);

  // Scroll the search-jump target into view, then clear the highlight. Without
  // the scroll the row flashes off-screen when the jump lands below the fold.
  // Must run here rather than in the onJump handler: the owning mod was just
  // selected and its group just expanded, so the row only exists in the DOM
  // after that render commits — which is when a `flash`-keyed effect fires. The
  // row is found by walking `.row[data-key]` and matching the attribute rather
  // than building a selector string, so a key containing a quote or bracket
  // needs no escaping. `scrollIntoView` is guarded because jsdom omits it.
  useEffect(() => {
    if (!flash) return;
    const detail = document.getElementById('detail');
    if (detail) {
      const rows = detail.querySelectorAll('.row[data-key]');
      for (let i = 0; i < rows.length; i++) {
        const r = rows[i];
        if (r instanceof HTMLElement && r.getAttribute('data-key') === flash.key) {
          if (r.scrollIntoView) r.scrollIntoView({ block: 'center' });
          break;
        }
      }
    }
    const t = setTimeout(() => setFlash(null), FLASH_MS);
    return () => clearTimeout(t);
  }, [flash]);

  // Timers outlive a render; an unmount mid-flight must not fire into a
  // torn-down tree.
  useEffect(
    () => () => {
      if (fadeTimer.current !== null) clearTimeout(fadeTimer.current);
    },
    [],
  );

  const changes = sessionDiff(baseline, mods);
  const needsUpdate = deriveNeedsUpdate(
    hostVersion,
    allViews.filter((v) => v.targetVersion).map((v) => ({
      targetVersion: v.targetVersion,
      // Views name themselves by their owning mod's title when one is loaded,
      // then the raw manifest `mod` string, then the view id.
      label: homeModCaption(v, mods) || v.mod || v.id,
    })),
    mods.map((m) => ({ targetVersion: m.targetVersion, label: titleOf(m) })),
  );
  const versionTitle = needsUpdate.outdated
    ? tr('newerExpectedBy', 'A newer OSF UI is expected by: {mods}', {
        mods: needsUpdate.wanting.join(', '),
      })
    : tr('version', 'OSF UI version');

  // The i18n generation has no other consumer; reading it here is what makes a
  // locale change repaint every translated string.
  void i18nSeq;

  const hudOn = (v: ViewRecord): boolean => {
    const override = hudOverride[v.id];
    return override === undefined ? v.open === true : override;
  };

  const applyPreset = (mod: ModRecord, preset: PresetRecord) => {
    const entries: Array<[string, SettingValue]> = [];
    for (const key in preset.values) entries.push([key, preset.values[key] as SettingValue]);
    applyLocal(mod.id, entries);
    for (const [key, value] of entries) setValue(mod.id, key, value);
    // Native does not re-broadcast settings.data on a set; the controls repaint
    // from the local model change alone, and group collapse survives.
    toast(
      tr.plural(
        'presetApplied',
        entries.length,
        'Applied "{preset}" ({count} setting)',
        'Applied "{preset}" ({count} settings)',
        { preset: preset.label },
      ),
    );
  };

  const revertOne = (c: { modId: string; key: string; old: SettingValue | undefined }) => {
    // A baseline of `undefined` means the key had no stored value when the
    // visit began. It goes through to `settings.set` as-is: JSON.stringify
    // drops the field and native refuses the write. Inventing a value here
    // would write something the visit never had.
    applyLocal(c.modId, [[c.key, c.old as SettingValue]]);
    setValue(c.modId, c.key, c.old as SettingValue);
  };

  const runAction = (command: string, modId: string, key: string | undefined) =>
    // The plugin command settles as ui.result (ok:true = delivered to the
    // plugin's handler; richer replies are the plugin's own message types).
    // Timeout / unknown-command / no-bridge all reject.
    bridge
      .request(command, { mod: modId, key }, { timeoutMs: ACTION_TIMEOUT_MS })
      .then((msg) => {
        const payload = msg.payload as { message?: unknown } | undefined;
        return payload && typeof payload.message === 'string' ? payload.message : null;
      });

  return (
    <>
      <Scrim />

      <div class="settings">
        <aside class="rail">
          <div class="rail-head">
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
                <div class="osf-eyebrow brand-sub">{tr('controlDeck', 'CONTROL DECK')}</div>
              </div>
              {/* Version from the runtime.ready handshake (empty until it
                  arrives). Badge turns yellow and the tag appears when an
                  installed mod or view targets a newer OSF UI than this one; the
                  tooltip names who is asking. The tag links to the Nexus page;
                  in-game the host intercepts target="_blank" and opens the
                  default browser. */}
              <div class="version-stack">
                <span
                  id="plugin-version"
                  class={needsUpdate.outdated ? 'version-badge is-outdated' : 'version-badge'}
                  title={versionTitle}
                >
                  {hostVersion ? `v${hostVersion}` : ''}
                </span>
                <a
                  id="needs-update-tag"
                  class="needs-update-tag"
                  hidden={!needsUpdate.outdated}
                  title={versionTitle}
                  href={NEXUS_PAGE_URL}
                  target="_blank"
                  rel="noreferrer"
                >
                  {tr('needsUpdate', 'Needs update')}
                </a>
              </div>
            </div>

            <SearchBox
              id="filter"
              value={filter}
              onInput={setFilter}
              placeholder={tr('searchPlaceholder', 'Search mods & settings')}
              ariaLabel={tr('searchLabel', 'Search mods and settings')}
              kbd="Ctrl F"
              keyshortcuts="Control+F"
              inputClass="filter"
              inputRef={filterInput}
            />

            <div class="rail-meta">
              <span>{tr('installedSystems', 'Installed systems')}</span>
              <span>{tr('configure', 'Configure')}</span>
            </div>
          </div>

          <Rail
            mods={mods}
            views={views}
            loadErrors={loadErrors}
            query={query}
            selectedId={selectedId}
            tr={tr}
            assetRoots={assetRoots}
            onSelect={selectMod}
          />

          <div class="rail-foot">
            {/* Write-behind save feedback: "Saving…" until the runtime confirms
                the disk write (settings.persisted), then "Saved" fading out. */}
            <span
              id="save-state"
              class={['save-state', 'osf-eyebrow', ...save.classes].join(' ')}
              aria-live="polite"
            >
              {save.label === 'saved' ? tr('saved', 'Saved') : save.label === 'saving' ? tr('saving', 'Saving…') : ''}
            </span>
            {/* Everything saves automatically, so this must not read as
                "unsaved changes": it is undo over what you touched since
                opening settings this time. */}
            <button
              id="session-chip"
              type="button"
              class="osf-btn osf-btn--ghost osf-btn--sm session-chip"
              style={changes.length ? '' : 'display:none'}
              onClick={() => setUndoOpen(true)}
            >
              {tr('undoChanges', 'Undo changes ({count})', { count: changes.length })}
            </button>
            <button
              id="close"
              type="button"
              class="osf-btn osf-btn--ghost osf-btn--sm osf-close"
              aria-keyshortcuts="Escape"
              onClick={() => sendCommand('close')}
            >
              <span>{tr('exit', 'Exit control deck')}</span>
              <kbd>Esc</kbd>
            </button>
          </div>
        </aside>

        <Detail
          mods={mods}
          views={views}
          diagnosticViews={allViews}
          query={query}
          selectedId={selectedId}
          hostVersion={hostVersion}
          tr={tr}
          assetRoots={assetRoots}
          collapsed={collapsed}
          onToggleGroup={(key, next) => setCollapsed((c) => ({ ...c, [key]: next }))}
          capturing={capture.capturing}
          flash={flash}
          hudOn={hudOn}
          onOpenView={(viewId) => sendCommand('menu.open', { view: viewId })}
          onHudToggle={(viewId, next) => {
            setHudOverride((o) => ({ ...o, [viewId]: next }));
            sendCommand(next ? 'hud.show' : 'hud.hide', { view: viewId });
          }}
          onRenderStatsToggle={(viewId, next) =>
            sendCommand('renderStats.set', { view: viewId, enabled: next })
          }
          onCommit={commit}
          onResetSetting={(modId, key) => requestReset(modId, key)}
          onResetMod={(modId) => requestReset(modId, null)}
          onBeginCapture={capture.begin}
          onApplyPreset={applyPreset}
          onJump={(r) => {
            // Clearing the filter is what switches the detail pane back to the
            // settings page; then select the owner, expand the setting's group,
            // and flash the row.
            setFilter('');
            setQuery('');
            setSelectedId(r.modId);
            const mod = modsRef.current.find((m) => m.id === r.modId);
            const groups = (mod && mod.schema && mod.schema.groups) || [];
            const index = groups.findIndex((g) =>
              (g.settings || []).some((s) => (s as { key?: unknown }).key === r.key),
            );
            if (index >= 0) setCollapsed((c) => ({ ...c, [`${r.modId}::g${index}`]: false }));
            setFlash({ modId: r.modId, key: r.key });
          }}
          onToast={toast}
          runAction={runAction}
          applyAccent={(el, hex) => bridge.applyAccent(el, hex)}
        />
      </div>

      {undoOpen ? (
        <UndoPanel
          changes={changes}
          tr={tr}
          onRevert={(c) => {
            revertOne(c);
            setUndoOpen(false);
          }}
          onRevertAll={(all) => {
            all.forEach(revertOne);
            setUndoOpen(false);
          }}
          onClose={() => setUndoOpen(false)}
        />
      ) : null}

      <ToastStack id="toast" entries={toasts.entries} />
    </>
  );
}
