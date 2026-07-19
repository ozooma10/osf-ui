// useCapture.ts — the key-rebind capture state machine.
//
// Ports `beginCapture` / `finishCapture` / `localKeyConflicts`
// (settings/main.legacy.js:1543-1633).
//
// ONE CAPTURE AT A TIME, VIEW-WIDE. Not per row: the runtime can only have one
// key grab armed, and a second arm anywhere rejects with `capture-busy`
// (protocol 1.0). So the armed row is held here, at the view's root, and each
// KeyField renders `listening` by comparing itself against it.
//
// WHY THE RUNTIME CAPTURES, NOT A keydown LISTENER: the user may be rebinding
// the very key that opens this overlay. Native sees the press before its own
// hotkey dispatch, so `settings.captureKey` is what makes "press F10 to rebind
// F10" work instead of closing the surface. The request is issued with
// `timeoutMs: 0` — the user may think for as long as they like, and the reply
// itself is what settles it (Escape or a refusal comes back `cancelled`).
//
// NOT PORTED: `capturing.timer`. Legacy destructured a `timer` field out of the
// capture record and cleared it (main.legacy.js:1615-1616), but nothing ever
// assigned one — a leftover from a pre-`timeoutMs:0` design. Dead on arrival.

import { useRef, useState } from 'preact/hooks';
import type { Bridge } from '@lib/bridge';
import { domKeyName } from '@lib/keybinds/domKeyName';
import { titleOf, type ModRecord } from '@lib/settings/rail';

/** The armed rebind: which mod's which key. */
export interface CaptureTarget {
  modId: string;
  key: string;
}

/**
 * What `finish` accepts: a real `settings.captured` payload, or the synthetic
 * cancel built locally when the request rejects. Every field is optional
 * because both shapes flow through one function.
 */
export interface CapturePayload {
  name?: string | undefined;
  cancelled?: boolean | undefined;
  conflicts?: Array<{ mod?: string; key?: string; title?: string }> | undefined;
}

export interface CaptureApi {
  /** The armed target, or null. A KeyField is `listening` when it matches. */
  capturing: CaptureTarget | null;
  /** True while ANY capture is armed — the Escape handler's guard. */
  isCapturing: () => boolean;
  begin: (modId: string, key: string) => void;
  /** Settle a capture. Idempotent — a second delivery no-ops. */
  finish: (payload: CapturePayload | null | undefined) => void;
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

/**
 * Standalone-only preview of `SettingsStore::ConflictsFor`: the OTHER key
 * settings whose CURRENT value is the captured name. In game the real list
 * arrives inside `settings.captured`, resolved by virtual-key code; here a
 * string compare is close enough to demo the warning (main.legacy.js:1585-1598).
 */
export function localKeyConflicts(
  mods: ModRecord[],
  name: string,
  modId: string,
  key: string,
): Array<{ mod: string; key: string; title: string }> {
  const others: Array<{ mod: string; key: string; title: string }> = [];
  for (const m of mods) {
    for (const g of (m.schema && m.schema.groups) || []) {
      for (const s of g.settings || []) {
        const item = s as { type?: unknown; key?: unknown } | null;
        if (!item || item.type !== 'key' || typeof item.key !== 'string') continue;
        if ((m.values || {})[item.key] !== name) continue;
        // Skip the setting being rebound — it is trivially "already bound" to
        // whatever it is being set to.
        if (m.id === modId && item.key === key) continue;
        others.push({ mod: m.id, key: item.key, title: titleOf(m) });
      }
    }
  }
  return others;
}

export function useCapture(opts: CaptureOptions): CaptureApi {
  const { bridge, modsRef, onCommit, toast, tr } = opts;

  const [capturing, setCapturingState] = useState<CaptureTarget | null>(null);
  // Mirrored into a ref because the bridge callbacks below are created inside a
  // render and settle much later, when `capturing` in their closure is stale.
  const capturingRef = useRef<CaptureTarget | null>(null);
  const setCapturing = (next: CaptureTarget | null) => {
    capturingRef.current = next;
    setCapturingState(next);
  };

  const finish = (payload: CapturePayload | null | undefined) => {
    const current = capturingRef.current;
    // Idempotent: the awaited request AND the belt-and-braces
    // `settings.captured` subscription both call this.
    if (!current) return;
    const { modId, key } = current;
    setCapturing(null);

    // A cancel (Escape, a refusal, a nameless reply) restores the button by
    // simply re-rendering it from the model — legacy stashed the previous
    // label in `prev` because it was mutating textContent directly.
    if (!payload || payload.cancelled || !payload.name) return;
    const name = payload.name;

    // Live-warn (mcm-design §9): the runtime checked the captured key against
    // every other binding BEFORE this commit lands, so the warning shows now
    // rather than after the settings.data round trip that repaints the row
    // badges. Informational — the bind stands either way.
    if (Array.isArray(payload.conflicts) && payload.conflicts.length) {
      const others = [...new Set(payload.conflicts.map((c) => c.title || c.mod))];
      toast(
        tr('capturedAlsoBound', '{key} is also bound by: {others}', {
          key: name,
          others: others.join(', '),
        }),
        'warn',
      );
    }

    // The mod/key we ARMED with, never the payload's — a mis-correlated reply
    // cannot then write into a different mod's values.
    onCommit(modId, key, name);
  };

  const begin = (modId: string, key: string) => {
    if (capturingRef.current) return; // one at a time, view-wide
    setCapturing({ modId, key });

    if (bridge.available()) {
      bridge
        .request('settings.captureKey', { mod: modId, key }, { timeoutMs: 0 })
        .then((msg) => finish(msg.payload as CapturePayload))
        .catch((err: unknown) => {
          // Only if OUR arm is still live: a rejection arriving after the
          // capture was settled some other way must not toast.
          const live = capturingRef.current;
          if (!live || live.modId !== modId || live.key !== key) return;
          finish({ cancelled: true });
          const e = err as { code?: unknown } | null;
          const code = e && typeof e.code === 'string' ? e.code : '';
          toast(
            code === 'capture-busy'
              ? tr('captureBusy', 'Another rebind is already listening.')
              : tr('captureNoResponse', "Rebinding didn't get a response from the runtime."),
            'warn',
          );
        });
      return;
    }

    // Standalone preview: no runtime to capture for us, so read the key off the
    // DOM. CAPTURE PHASE + preventDefault, so the press never reaches the
    // document-level Escape handler (which would close the surface mid-rebind).
    const onKey = (e: KeyboardEvent) => {
      window.removeEventListener('keydown', onKey, true);
      e.preventDefault();
      const name = domKeyName(e);
      const cancelled = e.key === 'Escape' || !name;
      finish({
        name,
        cancelled,
        conflicts: cancelled ? [] : localKeyConflicts(modsRef.current, name, modId, key),
      });
    };
    window.addEventListener('keydown', onKey, true);
  };

  return {
    capturing,
    isCapturing: () => capturingRef.current !== null,
    begin,
    finish,
  };
}
