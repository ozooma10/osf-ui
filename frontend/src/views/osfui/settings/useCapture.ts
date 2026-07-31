// Settings-specific adapter around the shared key-capture state machine.

import type { Bridge } from '@lib/bridge';
import { codeOf } from '@lib/protocol';
import { titleOf, type ModRecord } from '@lib/settings/rail';
import {
  useKeyCapture,
  type KeyCaptureApi,
  type KeyCaptureConflict,
} from '@ui/useKeyCapture';

export interface CaptureTarget {
  modId: string;
  key: string;
}


export interface CaptureApi extends Omit<KeyCaptureApi<CaptureTarget>, 'begin'> {
  begin: (modId: string, key: string) => void;
}

export interface CaptureOptions {
  bridge: Bridge;
  /** Live mods, for the standalone conflict preview. */
  modsRef: { current: ModRecord[] };
  /** Commit the captured name (the App's optimistic `commit`). */
  onCommit: (modId: string, key: string, name: string) => void;
  toast: (message: string, kind?: 'warn' | 'danger') => void;
  /** `tr` bound to chrome.settings. */
  tr: (address: string, english: string, vars?: Record<string, string | number>) => string;
}

/** Standalone approximation of the runtime's VK-resolved conflict lookup. */
export function localKeyConflicts(
  mods: ModRecord[],
  name: string,
  modId: string,
  key: string,
): Array<{ mod: string; key: string; title: string }> {
  const others: Array<{ mod: string; key: string; title: string }> = [];
  for (const mod of mods) {
    for (const group of (mod.schema && mod.schema.groups) || []) {
      for (const setting of group.settings || []) {
        const item = setting as { type?: unknown; key?: unknown } | null;
        if (!item || item.type !== 'key' || typeof item.key !== 'string') continue;
        if ((mod.values || {})[item.key] !== name) continue;
        if (mod.id === modId && item.key === key) continue;
        others.push({ mod: mod.id, key: item.key, title: titleOf(mod) });
      }
    }
  }
  return others;
}

export function useCapture(options: CaptureOptions): CaptureApi {
  const capture = useKeyCapture<CaptureTarget>({
    bridge: options.bridge,
    requestFields: ({ modId, key }) => ({ mod: modId, key }),
    onCommit: ({ modId, key }, name) => options.onCommit(modId, key, name),
    onConflicts: (_target, name, conflicts) => {
      const others = [...new Set(conflicts.map((conflict) => conflict.title || conflict.mod))];
      options.toast(
        options.tr('capturedAlsoBound', '{key} is also bound by: {others}', {
          key: name,
          others: others.join(', '),
        }),
        'warn',
      );
    },
    onError: (error) => {
      options.toast(
        codeOf(error) === 'capture-busy'
          ? options.tr('captureBusy', 'Another rebind is already listening.')
          : options.tr('captureNoResponse', "Rebinding didn't get a response from the runtime."),
        'warn',
      );
    },
    standaloneConflicts: ({ modId, key }, name): KeyCaptureConflict[] =>
      localKeyConflicts(options.modsRef.current, name, modId, key),
  });

  return {
    ...capture,
    begin: (modId, key) => capture.begin({ modId, key }),
  };
}
