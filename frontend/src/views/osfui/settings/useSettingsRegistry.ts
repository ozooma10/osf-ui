
import { useEffect, useState } from 'preact/hooks';
import type { Bridge } from '@lib/bridge';
import { useLatest, useStateRef } from '@ui/useStateRef';
import type { ModRecord, ViewRecord } from '@lib/settings/rail';
import { EMPTY_HEALTH, readHealth, type HealthModel } from '@lib/settings/health';
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
  onViewsData: () => void;
}

export interface SettingsRegistry {
  mods: ModRecord[];
  modsRef: { current: ModRecord[] };
  keyboard: SettingsData['keyboard'];
  /** Catalog-visible views — the rail, Home and the per-mod view sections. */
  views: ViewRecord[];
  viewsRef: { current: ViewRecord[] };
  discoveredViews: ViewRecord[];
  health: HealthModel;
  /** From the bridge `ready` handshake; "" until it arrives. */
  osfuiReleaseVersion: string;
  baseline: Baseline;
  applyLocal: (modId: string, entries: Array<[string, SettingValue]>) => void;
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

  /** The session health snapshot behind the fixed System Health destination. */
  const [health, setHealth] = useState<HealthModel>(EMPTY_HEALTH);

  const [baseline, setBaseline, baselineRef] = useStateRef<Baseline>({});

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

  const captureBaseline = (list: ModRecord[]) => {
    let next = baselineRef.current;
    let changed = false;
    for (const mod of list) {
      const values = mod.values || {};
      const seeded = seedBaseline(next, mod.id, Object.keys(values), values, true);
      if (seeded) {
        next = seeded;
        changed = true;
      }
    }
    if (changed) setBaseline(next);
  };

  useEffect(() => {
    const offSettings = bridge.state('osfui/settings', (data) => {
      const list = (data?.mods || []) as ModRecord[];
      setMods(list);
      setKeyboard(data && typeof data.keyboard === 'object' ? data.keyboard : undefined);
      captureBaseline(list);
    });

    const offHealth = bridge.state('osfui/diagnostics', (data) => setHealth(readHealth(data)));

    const offViews = bridge.state('osfui/views', (data) => {
      const all = (data?.views || []) as ViewRecord[];
      setDiscoveredViews(all.filter((v) => v && v.id));
      setViews(all.filter((v) => v && v.hub !== false));
      onViewsDataRef.current();
    });

    const offI18n = bridge.state('osfui/i18n', () => {
      if (modsRef.current.length || viewsRef.current.length) setI18nSeq((n) => n + 1);
    });

    const offChanged = bridge.on('settings.changed', (p) => {
      if (typeof p.mod !== 'string' || typeof p.key !== 'string') return;
      const modId = p.mod;
      const key = p.key;
      const mod = modsRef.current.find((m) => m.id === modId);
      if (!mod) return;

      const seeded = seedBaseline(baselineRef.current, modId, [key], mod.values || {});
      if (seeded) setBaseline(seeded);

      const changedSetting = findSettingInMod(mod, key);
      if (changedSetting && changedSetting.type === 'key') {
        const withValue = patchModValues(modsRef.current, modId, {
          [key]: p.value as SettingValue,
        });
        setMods(
          applyConflictUpdate(withValue, modId, key, Array.isArray(p.conflicts) ? p.conflicts : []),
        );
        return;
      }

      if (sameValue((mod.values || {})[key], p.value)) {
        return;
      }
      setMods(patchModValues(modsRef.current, modId, { [key]: p.value as SettingValue }));
    });

    void bridge
      .ready()
      .then((info) => setOsfuiReleaseVersion(info.version || ''))
      .catch(() => {});

    return () => {
      offSettings();
      offHealth();
      offViews();
      offI18n();
      offChanged();
    };
  }, [bridge]);

  return {
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
    clearBaseline: () => setBaseline({}),
  };
}
