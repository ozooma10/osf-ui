// useSettingsRegistry — everything the Mods surface KNOWS, separated from
// everything it is currently DOING.
//
// What lives here: the two registries the pane paints from (`settings.data`
// schemas and `views.data` catalog entries), the health snapshot, the host
// version, the undo baseline, and the bridge subscriptions that keep all of them
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
import { EMPTY_HEALTH, readHealth, type HealthModel } from '@lib/settings/diagnostics';
import { applyConflictUpdate } from '@lib/settings/conflicts';
import {
  findSettingInMod,
  patchModValues,
  sameValue,
  seedBaseline,
  type Baseline,
} from '@lib/settings/modified';
import type { SettingValue } from '@sdk';

export interface SettingsRegistryOptions {
  bridge: Bridge;
  /**
   * A fresh `views.data` push landed. It is the authority on HUD open state, so
   * the App drops its optimistic switch overrides here. Read through a latest-ref
   * rather than closed over, so the caller may pass a new function every render.
   */
  onViewsData: () => void;
}

export interface SettingsRegistry {
  mods: ModRecord[];
  modsRef: { current: ModRecord[] };
  /** Hub-visible views — the rail, Home and the per-mod surface sections. */
  views: ViewRecord[];
  viewsRef: { current: ViewRecord[] };
  /**
   * The COMPLETE discovery catalog, including `hub:false`, debug-only and
   * not-yet-loaded entries. Diagnostics keeps these so a mod author can prove a
   * view registered without first making it visible in normal menus.
   */
  discoveredViews: ViewRecord[];
  health: HealthModel;
  /** From the `runtime.ready` handshake; "" until it arrives. */
  hostVersion: string;
  baseline: Baseline;
  baselineRef: { current: Baseline };
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
  const [views, setViews, viewsRef] = useStateRef<ViewRecord[]>([]);
  const [discoveredViews, setDiscoveredViews] = useState<ViewRecord[]>([]);
  const [hostVersion, setHostVersion] = useState('');

  /**
   * The session health snapshot. Settings-file load failures used to be a rail
   * banner fed by `settings.data`'s `loadErrors`; they are health issues like
   * everything else now, so this is the single model behind the pinned
   * destination, the rail badge, the per-mod severity markers and the failed
   * card deep links.
   */
  const [health, setHealth] = useState<HealthModel>(EMPTY_HEALTH);

  /**
   * `baseline[modId][key]` — the value when this visit began. Drives the undo
   * chip and the revert panel. Kept across data refreshes, so a reset or a
   * preset re-broadcast does not lose undo history.
   */
  const [baseline, setBaseline, baselineRef] = useStateRef<Baseline>({});

  // Bumped on every `i18n.data` push. Not exposed: this is the hook's own state,
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
    const offSettings = bridge.on('settings.data', (p) => {
      const list = (p.mods || []) as ModRecord[];
      setMods(list);
      // `p.loadErrors` is deliberately ignored: the same failures arrive as
      // `diagnostics.data` issues, which carry severity, an occurrence count and
      // actions. Reading both would double-report them.
      captureBaseline(list);
    });

    const offDiagnostics = bridge.on('diagnostics.data', (p) => setHealth(readHealth(p)));

    const offViews = bridge.on('views.data', (p) => {
      const all = (p.views || []) as ViewRecord[];
      setDiscoveredViews(all.filter((v) => v && v.id));
      setViews(all.filter((v) => v && v.hub !== false));
      onViewsDataRef.current();
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

    const requestCatalogs = () => {
      if (!bridge.available()) return;
      bridge.emit('settings.get');
      bridge.emit('views.get');
      // Same subscribe-on-read contract. A host that predates protocol 1.4
      // answers ui.error and the pane simply stays nominal.
      bridge.emit('diagnostics.get');
    };

    // The initial reads must not be gated on `ready`. `runtime.ready` is a
    // one-shot greeting emitted at runtime init, which can be long before this
    // page's transport can carry it (the WebView2 host is a separate process
    // that starts on the first game tick); gating on it left the Mods surface
    // permanently empty whenever the greeting was missed. The gets are
    // idempotent and also subscribe to the change pushes.
    requestCatalogs();

    // A page reload can also run before the injected transport reports itself
    // available, then receive `runtime.ready` moments later. Reissue the
    // idempotent reads on that edge so a populated version badge can never sit
    // above an empty deck merely because the first availability check lost the
    // race. A missed greeting remains safe because the immediate reads above do
    // not depend on it.
    void bridge.ready().then((info) => {
      setHostVersion(info.version || '');
      requestCatalogs();
    });

    return () => {
      offSettings();
      offDiagnostics();
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
    views,
    viewsRef,
    discoveredViews,
    health,
    hostVersion,
    baseline,
    baselineRef,
    applyLocal,
    clearBaseline: () => setBaseline({}),
  };
}
