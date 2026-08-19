
import { useEffect, useMemo, useRef, useState } from 'preact/hooks';
import { windowBridge, type Bridge } from '@lib/bridge';
import { makeTranslator } from '@lib/i18n';
import { NEXUS_PAGE_URL } from '@lib/links';
import { codeOf } from '@lib/protocol';
import { BrandEmblem } from '@ui/BrandEmblem';
import { useLatest, useStateRef } from '@ui/useStateRef';
import { Scrim } from '@ui/Scrim';
import { SearchBox } from '@ui/SearchBox';
import { ToastStack, useToasts } from '@ui/Toast';
import { ACTION_TIMEOUT_MS } from '@ui/ActionButton';
import type { AssetRoots } from '@lib/settings/assets';
import {
  HEALTH_ID,
  HOME_ID,
  railNodes,
  titleOf,
  type ModRecord,
  type ViewRecord,
} from '@lib/settings/rail';
import { makeLabeler } from '@lib/keybinds/labels';
import { sessionDiff } from '@lib/settings/modified';
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
  saveStateFaded,
  saveStatePending,
  saveStatePersisted,
  type SaveState,
  type SaveTransition,
} from '@lib/saveState';
import type { SettingValue } from '@sdk';
import { pageIdForGroup } from '@lib/settings/pages';
import { Detail, groupKey } from './Detail';
import { HealthItem, Rail } from './Rail';
import { UndoPanel } from './UndoPanel';
import { homeModCaption } from './Home';
import { useCapture } from './useCapture';
import { useSettingsRegistry } from './useSettingsRegistry';
import type { PresetRecord } from './Presets';

const FILTER_DEBOUNCE_MS = 120;
/** How long a search-jump target stays highlighted. */
const FLASH_MS = 1200;

export interface AppProps {
  bridge?: Bridge;
  assetRoots?: AssetRoots;
}

export function App({ bridge = windowBridge, assetRoots }: AppProps) {
  const tr = useMemo(() => makeTranslator(bridge, 'chrome.settings'), [bridge]);

  const [hudOverride, setHudOverride] = useState<Record<string, boolean>>({});

  const [autoStartPending, setAutoStartPending] = useState<Record<string, boolean>>({});

  const registry = useSettingsRegistry({
    bridge,
    onViewsData: () => {
      setHudOverride({});
      setAutoStartPending({});
    },
  });
  const {
    mods,
    modsRef,
    keyboard,
    views,
    viewsRef,
    discoveredViews,
    health,
    osfuiReleaseVersion,
    baseline,
    applyLocal,
  } = registry;

  /** Issue System Health should expand and scroll to, from a deep link. */
  const [focusIssueId, setFocusIssueId] = useState<string | null>(null);

  const [selectedId, setSelectedId, selectedIdRef] = useStateRef<string | null>(null);

  const [filter, setFilter, filterRef] = useStateRef('');
  const [query, setQuery] = useState('');
  const queryRef = useLatest(query);

  const [collapsed, setCollapsed] = useState<Record<string, boolean>>({});
  const [activePages, setActivePages] = useState<Record<string, string>>({});
  const [undoOpen, setUndoOpen] = useState(false);
  const [flash, setFlash] = useState<{ modId: string; key: string } | null>(null);
  const [openCooldownEpoch, setOpenCooldownEpoch] = useState(0);

  const [save, setSave, saveRef] = useStateRef<SaveState>(initialSaveState);
  const fadeTimer = useRef<ReturnType<typeof setTimeout> | null>(null);
  const applySave = (t: SaveTransition) => {
    setSave(t.state);
    if (t.cancelFade && fadeTimer.current !== null) {
      clearTimeout(fadeTimer.current);
      fadeTimer.current = null;
    }
    if (t.scheduleFadeMs !== null) {
      fadeTimer.current = setTimeout(() => {
        setSave(saveStateFaded(saveRef.current));
      }, t.scheduleFadeMs);
    }
  };

  const padRef = useRef<PadButtonState>(initialPadButtonState);

  const pendingHashSelect = useRef<string | null>(null);
  const hashRead = useRef(false);
  if (!hashRead.current) {
    hashRead.current = true;
    const m = /^#mod=(.+)$/.exec((typeof location !== 'undefined' && location.hash) || '');
    if (m && m[1]) {
      try {
        pendingHashSelect.current = decodeURIComponent(m[1]);
      } catch {
        pendingHashSelect.current = null;
      }
    }
  }

  const toasts = useToasts();
  const toastRef = useLatest(toasts);
  const toast = (message: string, kind?: 'warn' | 'danger') => {
    if (kind === undefined) toastRef.current.push(message);
    else toastRef.current.push(message, kind);
  };

  const filterInput = useRef<HTMLInputElement | null>(null);

  /** A one-way endpoint. Nothing settles; wanting an outcome means requestOp. */
  const sendEndpoint = (endpoint: string, fields?: Record<string, unknown>) => {
    if (bridge.available()) bridge.send(endpoint, fields);
  };

  const requestOp = (endpoint: string, payload?: Record<string, unknown>) => {
    if (!bridge.available()) return;
    void bridge.request(endpoint, payload).catch((err: unknown) => {
      const code = codeOf(err);
      toast(tr('actionFailed', 'Could not complete that{code}', {
        code: code ? ` (${code})` : '',
      }), 'danger');
    });
  };

  const setValue = (
    modId: string,
    key: string,
    value: SettingValue,
    onRejected?: () => void,
  ) => {
    if (!bridge.available()) return;
    applySave(saveStatePending(saveRef.current, modId));
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
      onRejected?.();
    });
  };

  const pushValues = (modId: string, entries: Array<[string, SettingValue]>) => {
    const values = modsRef.current.find((m) => m.id === modId)?.values || {};
    const previous = new Map(entries.map(([key]) => [key, values[key]]));
    applyLocal(modId, entries);
    for (const [key, value] of entries) {
      setValue(modId, key, value, () => {
        applyLocal(modId, [[key, previous.get(key) as SettingValue]]);
      });
    }
  };

  /** Reset one key, or the whole mod when `key` is null. Same ordering rule. */
  const requestReset = (modId: string, key: string | null) => {
    if (!bridge.available()) return;
    applySave(saveStatePending(saveRef.current, modId));
    bridge
      .request('settings.reset', key ? { mod: modId, key } : { mod: modId })
      .catch((err: unknown) => {
        const code = codeOf(err);
        toast(tr('resetFailed', 'Reset failed{code}', { code: code ? ` (${code})` : '' }), 'danger');
        applySave(saveStateAbandon(saveRef.current, modId));
      });
  };

  /** A user commit: local model first, then the wire. */
  const commit = (modId: string, key: string, value: SettingValue) => {
    pushValues(modId, [[key, value]]);
  };

  const selectMod = (id: string) => {
    if (id === HEALTH_ID) {
      setFilter('');
      setQuery('');
    } else {
      setFocusIssueId(null);
    }
    setSelectedId(id);
  };

  const openIssue = (issueId: string) => {
    setFilter('');
    setQuery('');
    setFocusIssueId(issueId);
    setSelectedId(HEALTH_ID);
  };

  useEffect(() => {
    const nodes = railNodes({ mods, views }, '');
    const ids = nodes.filter((n) => n.kind === 'entry').map((n) => (n as { entry: { id: string } }).entry.id);
    if (!ids.length) return; // no mod entries

    const pending = pendingHashSelect.current;
    if (pending && (pending === HOME_ID || pending === HEALTH_ID || ids.includes(pending))) {
      pendingHashSelect.current = null; // honoured once; later pushes must not override clicks
      setSelectedId(pending);
      return;
    }
    const current = selectedIdRef.current;
    if (current !== HOME_ID && current !== HEALTH_ID && (current === null || !ids.includes(current))) {
      setSelectedId(HOME_ID);
    }
  }, [mods, views]);

  useEffect(() => {
    const t = setTimeout(() => setQuery(filter.trim().toLowerCase()), FILTER_DEBOUNCE_MS);
    return () => clearTimeout(t);
  }, [filter]);

  const labeler = useMemo(() => makeLabeler(keyboard), [keyboard]);

  const capture = useCapture({
    bridge,
    modsRef,
    onCommit: commit,
    toast,
    tr,
  });


  useEffect(() => {
    const offCaptured = bridge.on('settings.captured', (p) => capture.finish(p as never));

    const offPersisted = bridge.on('settings.persisted', (p) => {
      applySave(saveStatePersisted(saveRef.current, p.mod));
    });

    const offVisibility = bridge.on('ui.visibility', (p) => {
      const intent = reduceVisibility(
        { selectedId: selectedIdRef.current ?? '', filter: filterRef.current },
        p,
      );
      // A focus handoff preserves the visit, but launch feedback belongs to
      // the view we just left and must not survive when Settings is revealed.
      if (p.visible) setOpenCooldownEpoch((epoch) => epoch + 1);
      if (!intent.clearBaseline) return; // hide edge
      registry.clearBaseline();
      if (intent.reselect) {
        // "Open the deck", not "resume where a past visit left off".
        setFilter('');
        setQuery('');
        setSelectedId(intent.state.selectedId);
      }
      setFocusIssueId(null);
      setUndoOpen(false);
      const padnav = (window as { padnav?: { reset?: () => void } }).padnav;
      if (padnav && padnav.reset) padnav.reset();
    });

    const offGamepad = bridge.on('ui.gamepad', (p) => {
      const nodes = railNodes(
        { mods: modsRef.current, views: viewsRef.current },
        queryRef.current,
      );
      // LB/RB walk every focusable rail row, fixed destinations included.
      const railIds = nodes
        .filter((n) => n.kind === 'health' || n.kind === 'home' || n.kind === 'entry')
        .map((n) =>
          n.kind === 'health'
            ? HEALTH_ID
            : n.kind === 'home'
              ? HOME_ID
              : (n as { entry: { id: string } }).entry.id,
        );
      const intent = reduceGamepad(padRef.current, p, {
        railIds,
        selectedId: selectedIdRef.current ?? '',
        modalOpen: undoOpenRef.current,
      });
      padRef.current = intent.state;
      if (intent.select !== null) selectMod(intent.select);
    });

    if (bridge.available()) sendEndpoint('osfui.handleBack', { handle: true });

    return () => {
      offCaptured();
      offPersisted();
      offVisibility();
      offGamepad();
    };
    // eslint-disable-next-line react-hooks/exhaustive-deps -- registered once
  }, [bridge]);

  // `undoOpen` is read from the gamepad subscription's stale closure.
  const undoOpenRef = useLatest(undoOpen);

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
      if ((e.key === 'Escape' || e.keyCode === 27) && !e.defaultPrevented && !capture.isCapturing()) {
        // Peel the undo panel first; only a bare Escape closes the view.
        if (undoOpenRef.current) {
          setUndoOpen(false);
          return;
        }
        sendEndpoint('close');
      }
    };
    document.addEventListener('keydown', onKeyDown);
    return () => document.removeEventListener('keydown', onKeyDown);
    // eslint-disable-next-line react-hooks/exhaustive-deps -- reads only refs
  }, [bridge]);

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

  useEffect(
    () => () => {
      if (fadeTimer.current !== null) clearTimeout(fadeTimer.current);
    },
    [],
  );

  const changes = sessionDiff(baseline, mods);
  const needsUpdate = deriveNeedsUpdate(
    osfuiReleaseVersion,
    discoveredViews
      .filter((v) => v.targetVersion)
      .map((v) => ({
        targetVersion: v.targetVersion,
        label: homeModCaption(v, mods) || v.mod || v.id,
      })),
    mods.map((m) => ({ targetVersion: m.targetVersion, label: titleOf(m) })),
  );
  const versionTitle = needsUpdate.outdated
    ? tr('newerExpectedBy', 'A newer OSF UI is expected by: {mods}', {
        mods: needsUpdate.wanting.join(', '),
      })
    : tr('version', 'OSF UI release version');

  const hudOn = (v: ViewRecord): boolean => {
    const override = hudOverride[v.id];
    return override === undefined ? v.open === true : override;
  };

  const autoStartOf = (v: ViewRecord): boolean => {
    const pending = autoStartPending[v.id];
    return pending === undefined ? v.autoStart === true : pending;
  };
  const autoStartBusy = (v: ViewRecord): boolean => autoStartPending[v.id] !== undefined;

  const setViewAutoStart = (viewId: string, enabled: boolean) => {
    if (!bridge.available()) return;
    setAutoStartPending((p) => ({ ...p, [viewId]: enabled }));
    bridge.request('osfui.setViewAutoStart', { view: viewId, enabled }).catch((err: unknown) => {
      const code = codeOf(err);
      setAutoStartPending((p) => {
        const next = { ...p };
        delete next[viewId];
        return next;
      });
      toast(
        tr('autoStartRejected', 'Start automatically was not saved{code}', {
          code: code ? ` (${code})` : '',
        }),
        'danger',
      );
    });
  };

  const applyPreset = (mod: ModRecord, preset: PresetRecord) => {
    const entries: Array<[string, SettingValue]> = [];
    for (const key in preset.values) entries.push([key, preset.values[key] as SettingValue]);
    pushValues(mod.id, entries);
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
    pushValues(c.modId, [[c.key, c.old as SettingValue]]);
  };

  const runAction = (requestEndpoint: string, modId: string, key: string | undefined) =>
    bridge
      .request<{ message?: unknown }>(requestEndpoint, { mod: modId, key }, { timeoutMs: ACTION_TIMEOUT_MS })
      .then((payload) => {
        return payload && typeof payload.message === 'string' ? payload.message : null;
      });

  return (
    <>
      <Scrim />

      <div class="settings">
        <aside class="rail">
          <div class="rail-head">
            <div class="brand">
              <BrandEmblem />
              <div class="brand-text">
                <div class="brand-line">
                  <span class="wordmark-osf">OSF</span>
                  <span class="wordmark-ui">UI</span>
                </div>
                {/* `controlDeck` is a frozen translation address; the default copy uses the canonical name. */}
                <div class="osf-eyebrow brand-sub">{tr('controlDeck', 'MOD SETTINGS')}</div>
              </div>
              {/* Version from the bridge `ready` handshake (empty until it
                  arrives). Badge turns yellow and the tag appears when an
                  installed mod or view targets a newer OSF UI than this one; the
                  tooltip names who is asking. The tag links to the Nexus page;
                  in-game the browser host intercepts target="_blank" and opens the
                  default browser. */}
              <div class="version-stack">
                <span
                  id="plugin-version"
                  class={needsUpdate.outdated ? 'version-badge is-outdated' : 'version-badge'}
                  title={versionTitle}
                >
                  {osfuiReleaseVersion ? `v${osfuiReleaseVersion}` : ''}
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
                <span
                  id="devmode-tag"
                  class="devmode-tag"
                  hidden={health.system?.devMode !== true}
                  title={tr('devModeHint', 'Developer mode is active: verbose logging, hot reload, F12 DevTools, and developer views are available.')}
                >
                  {tr('devMode', 'DEVELOPER MODE')}
                </span>
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

            <HealthItem
              health={health}
              selected={selectedId === HEALTH_ID}
              tr={tr}
              onSelect={selectMod}
            />

            <div class="rail-meta">
              {/* Compatibility catalog address; fallback copy uses the canonical mod noun. */}
              <span>{tr('installedSystems', 'Installed mods')}</span>
              <span>{tr('configure', 'Configure')}</span>
            </div>
          </div>

          <Rail
            mods={mods}
            views={views}
            health={health}
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
              onClick={() => sendEndpoint('close')}
            >
              <span>{tr('exit', 'Exit Mod Settings')}</span>
              <kbd>Esc</kbd>
            </button>
          </div>
        </aside>

        <Detail
          mods={mods}
          labeler={labeler}
          views={views}
          discoveredViews={discoveredViews}
          health={health}
          query={query}
          selectedId={selectedId}
          osfuiReleaseVersion={osfuiReleaseVersion}
          tr={tr}
          assetRoots={assetRoots}
          openCooldownEpoch={openCooldownEpoch}
          focusIssueId={focusIssueId}
          onOpenIssue={openIssue}
          collapsed={collapsed}
          onToggleGroup={(key, next) => setCollapsed((c) => ({ ...c, [key]: next }))}
          activePages={activePages}
          onSelectPage={(modId, pageId) => setActivePages((a) => ({ ...a, [modId]: pageId }))}
          capturing={capture.capturing}
          flash={flash}
          hudOn={hudOn}
          autoStartOf={autoStartOf}
          autoStartBusy={autoStartBusy}
          onAutoStartToggle={setViewAutoStart}
          onOpenView={(viewId) => requestOp('menu.open', { view: viewId })}
          onHudToggle={(viewId, next) => {
            setHudOverride((o) => ({ ...o, [viewId]: next }));
            requestOp(next ? 'menu.open' : 'menu.close', { view: viewId });
          }}
          onCommit={commit}
          onResetSetting={(modId, key) => requestReset(modId, key)}
          onResetMod={(modId) => requestReset(modId, null)}
          onBeginCapture={capture.begin}
          onApplyPreset={applyPreset}
          onJump={(r) => {
            setFilter('');
            setQuery('');
            setSelectedId(r.modId);
            const mod = modsRef.current.find((m) => m.id === r.modId);
            const groups = (mod && mod.schema && mod.schema.groups) || [];
            const index = groups.findIndex((g) =>
              (g.settings || []).some((s) => (s as { key?: unknown }).key === r.key),
            );
            if (index >= 0) {
              setCollapsed((c) => ({ ...c, [groupKey(r.modId, groups[index]!, index)]: false }));
              const pageId = pageIdForGroup(mod && mod.schema, index);
              if (pageId) setActivePages((a) => ({ ...a, [r.modId]: pageId }));
            }
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
