// useSettingsRegistry — everything the Mod Settings view KNOWS, separated from
// everything it is currently DOING.
//
// What lives here: the two registries the pane paints from (`osfui/settings`
// schemas and `osfui/views` catalog entries), the runtime version, the undo
// baseline, and the bridge subscriptions that keep all of them
// current. What deliberately does not: selection, filter, collapse state, the
// save indicator, key capture, gamepad and visibility handling. Those are about
// what the user is doing right now, they change on nearly every interaction, and
// none of them survives a fresh visit — whereas everything in here is authored
// elsewhere and merely observed.
//
// The subscriptions are registered ONCE per bridge, so every value a callback
// reads comes through a ref rather than a closed-over binding (see
// @ui/useStateRef). That constraint is the reason this file exists as a hook and
// not as a context or a store: the refs and the state they mirror have to be
// created together.
//
// Replies that resolve a `call()` are dispatched to these same subscriptions, so
// there is one render path regardless of who asked for the data.

import { useEffect, useState } from 'preact/hooks';
import type { Bridge } from '@lib/bridge';
import { useLatest, useStateRef } from '@ui/useStateRef';
import type { ModRecord, ViewRecord } from '@lib/settings/rail';
import { applyConflictUpdate } from '@lib/settings/conflicts';
import {
  findSettingInMod,
  patchModValues,
  sameValue,
  seedBaseline,
  type Baseline,
} from '@lib/settings/modified';
import type { SettingsData, SettingValue } from '@sdk';

export interface SettingsRegistryOptions {
  bridge: Bridge;
  /**
   * A fresh `osfui/views` state update landed. It is the authority on HUD open state, so
   * the App drops its optimistic switch overrides here. Read through a latest-ref
   * rather than closed over, so the caller may pass a new function every render.
   */
  onViewsData: () => void;
}

export interface SettingsRegistry {
  mods: ModRecord[];
  modsRef: { current: ModRecord[] };
  /**
   * Localized keycap labels for the current OS keyboard layout (the additive
   * `keyboard` block of `osfui/settings`), or undefined on older OSF UI runtimes and in
   * the preview — consumers fall back to raw key names.
   */
  keyboard: SettingsData['keyboard'];
  /** Catalog-visible views — the rail, Home and the per-mod view sections. */
  views: ViewRecord[];
  viewsRef: { current: ViewRecord[] };
  /**
   * The COMPLETE discovery catalog, including `hub:false`, debug-only and
   * not-yet-instantiated entries. The Discovered views inventory keeps these
   * so an author can prove a view was discovered without first making it visible
   * in normal menus.
   */
  discoveredViews: ViewRecord[];
  /** From the bridge `ready` handshake; "" until it arrives. */
  osfuiReleaseVersion: string;
  baseline: Baseline;
  /**
   * Apply values to the local model optimistically and record the pre-change
   * values against the session baseline. Batched over several keys because a
   * preset commits many at once and one state update per key would render the
   * pane N times.
   */
  applyLocal: (modId: string, entries: Array<[string, SettingValue]>) => void;
  /**
   * Drop the undo baseline. Called on the overlay's open edge, which is what
   * scopes undo to "since you opened settings" rather than to the whole game
   * session — the view keeps running while hidden.
   */
  clearBaseline: () => void;
}

export function useSettingsRegistry(opts: SettingsRegistryOptions): SettingsRegistry {
  const { bridge } = opts;
  const onViewsDataRef = useLatest(opts.onViewsData);

  const [mods, setMods, modsRef] = useStateRef<ModRecord[]>([]);
  const [keyboard, setKeyboard] = useState<SettingsData['keyboard']>(undefined);
  const [views, setViews, viewsRef] = useStateRef<ViewRecord[]>([]);
  const [discoveredViews, setDiscoveredViews] = useState<ViewRecord[]>([]);
  const [osfuiReleaseVersion, setOsfuiReleaseVersion] = useState('');

  /**
   * `baseline[modId][key]` — the value when this visit began. Drives the undo
   * chip and the revert panel. Kept across data refreshes, so a reset or a
   * preset re-broadcast does not lose undo history.
   */
  const [baseline, setBaseline, baselineRef] = useStateRef<Baseline>({});

  // Bumped whenever the locale catalog changes. Not exposed: this is the hook's own state,
  // so setting it already re-renders the caller, and `tr` reads through to
  // bridge.t per call rather than caching — there is nothing for a consumer to
  // subscribe to.
  const [, setI18nSeq] = useState(0);

  const applyLocal = (modId: string, entries: Array<[string, SettingValue]>) => {
    const mod = modsRef.current.find((m) => m.id === modId);
    if (!mod) return;
    const values = mod.values || {};

    const seeded = seedBaseline(baselineRef.current, modId, entries.map(([key]) => key), values);
    if (seeded) setBaseline(seeded);

    const patch: Record<string, SettingValue> = {};
    for (const [key, value] of entries) patch[key] = value;
    setMods(patchModValues(modsRef.current, modId, patch));
  };

  /**
   * Seed the baseline for every key of every mod that lacks one, on each data
   * arrival — after a visibility reset that amounts to a full snapshot at the
   * first arrival. Lazy per-key seeding on first change would instead make the
   * undo list report only keys touched through this pane, missing external
   * writers.
   */
  const captureBaseline = (list: ModRecord[]) => {
    let next = baselineRef.current;
    let changed = false;
    for (const mod of list) {
      const values = mod.values || {};
      // ensureEntry: a mod with no values still gets an entry, marking it
      // snapshotted. Only this whole-list capture wants that.
      const seeded = seedBaseline(next, mod.id, Object.keys(values), values, true);
      if (seeded) {
        next = seeded;
        changed = true;
      }
    }
    if (changed) setBaseline(next);
  };

  useEffect(() => {
    // Subscribing IS the read. Each handler runs immediately with the current
    // value and again on every change — on this document and on every later
    // one, because the OSF UI runtime replays state to each fresh document. Everything below
    // used to be four `*.get` request endpoints whose replies doubled as subscriptions,
    // re-sent on the bridge-ready edge in case the first attempt lost a race
    // with the transport coming up. None of that has anywhere left to live.
    const offSettings = bridge.state('osfui/settings', (data) => {
      const list = (data?.mods || []) as ModRecord[];
      setMods(list);
      setKeyboard(data && typeof data.keyboard === 'object' ? data.keyboard : undefined);
      captureBaseline(list);
    });

    const offViews = bridge.state('osfui/views', (data) => {
      const all = (data?.views || []) as ViewRecord[];
      setDiscoveredViews(all.filter((v) => v && v.id));
      setViews(all.filter((v) => v && v.hub !== false));
      onViewsDataRef.current();
    });

    const offI18n = bridge.state('osfui/i18n', () => {
      // A catalog arriving before any data must not force a paint of an empty
      // view.
      if (modsRef.current.length || viewsRef.current.length) setI18nSeq((n) => n + 1);
    });

    const offChanged = bridge.on('settings.changed', (p) => {
      // Native push for every committed value — our own commits echo back
      // (possibly clamped), and other writers (a sibling DLL, another mod view, a
      // preset applied in another view) stay in sync while the menu is open.
      if (typeof p.mod !== 'string' || typeof p.key !== 'string') return;
      const modId = p.mod;
      const key = p.key;
      const mod = modsRef.current.find((m) => m.id === modId);
      if (!mod) return;

      // Seed the baseline before overwriting, so an external writer's change is
      // undoable too.
      const seeded = seedBaseline(baselineRef.current, modId, [key], mod.values || {});
      if (seeded) setBaseline(seeded);

      const changedSetting = findSettingInMod(mod, key);
      if (changedSetting && changedSetting.type === 'key') {
        // Key-typed pushes carry the setting's recomputed `conflicts`
        // (protocol 0.5): apply both sides of the collision to the local model
        // instead of re-fetching the whole registry. Handled before the echo
        // check so our own rebind — already applied optimistically — still
        // updates the badges.
        const withValue = patchModValues(modsRef.current, modId, {
          [key]: p.value as SettingValue,
        });
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
      // The store disagrees with the local model (a native clamp, or an external
      // writer): adopt its value.
      setMods(patchModValues(modsRef.current, modId, { [key]: p.value as SettingValue }));
    });

    // The only thing still worth awaiting: the OSF UI release version, for the
    // badge. It cannot gate the data — and no longer needs to, since the
    // handshake is page-initiated and the replay follows it unconditionally.
    // Rejects standalone (no bridge), which is not an error worth surfacing.
    void bridge
      .ready()
      .then((info) => setOsfuiReleaseVersion(info.version || ''))
      .catch(() => {});

    return () => {
      offSettings();
      offViews();
      offI18n();
      offChanged();
    };
    // eslint-disable-next-line react-hooks/exhaustive-deps -- registered once
    // per bridge; everything else is read through a ref.
  }, [bridge]);

  return {
    mods,
    modsRef,
    keyboard,
    views,
    viewsRef,
    discoveredViews,
    osfuiReleaseVersion,
    baseline,
    applyLocal,
    clearBaseline: () => setBaseline({}),
  };
}
