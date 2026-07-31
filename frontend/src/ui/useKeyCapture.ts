// One runtime key grab may be armed per view. This hook owns correlation,
// cancellation, and the standalone DOM fallback; consumers own the model update
// and user-facing copy.

import { useEffect, useRef } from 'preact/hooks';
import type { Bridge } from '@lib/bridge';
import { domKeyName } from '@lib/keybinds/domKeyName';
import { useLatest, useStateRef } from './useStateRef';

export interface KeyCaptureConflict {
  mod?: string;
  key?: string;
  title?: string;
}

export interface KeyCapturePayload {
  name?: string | undefined;
  cancelled?: boolean | undefined;
  conflicts?: KeyCaptureConflict[] | undefined;
}

export interface KeyCaptureApi<T> {
  capturing: T | null;
  isCapturing: () => boolean;
  begin: (target: T) => void;
  /** Settle the live capture from an uncorrelated settings.captured push. */
  finish: (payload: KeyCapturePayload | null | undefined) => void;
}

export interface KeyCaptureOptions<T> {
  bridge: Bridge;
  requestFields: (target: T) => Record<string, unknown>;
  onCommit: (target: T, name: string) => void;
  onConflicts?: (target: T, name: string, conflicts: KeyCaptureConflict[]) => void;
  onError?: (error: unknown, target: T) => void;
  standaloneConflicts?: (target: T, name: string) => KeyCaptureConflict[];
}

export function useKeyCapture<T>(options: KeyCaptureOptions<T>): KeyCaptureApi<T> {
  const optionsRef = useLatest(options);
  const [capturing, setCapturing, capturingRef] = useStateRef<T | null>(null);
  const domCleanupRef = useRef<(() => void) | null>(null);

  const clearDomCapture = () => {
    domCleanupRef.current?.();
    domCleanupRef.current = null;
  };

  useEffect(() => clearDomCapture, []);

  const settle = (target: T, payload: KeyCapturePayload | null | undefined) => {
    if (capturingRef.current !== target) return;
    clearDomCapture();
    setCapturing(null);
    if (!payload || payload.cancelled || !payload.name) return;

    const conflicts = Array.isArray(payload.conflicts) ? payload.conflicts : [];
    if (conflicts.length) optionsRef.current.onConflicts?.(target, payload.name, conflicts);
    optionsRef.current.onCommit(target, payload.name);
  };

  const finish = (payload: KeyCapturePayload | null | undefined) => {
    const target = capturingRef.current;
    if (target !== null) settle(target, payload);
  };

  const begin = (target: T) => {
    if (capturingRef.current !== null) return;
    setCapturing(target);

    const opts = optionsRef.current;
    if (opts.bridge.available()) {
      opts.bridge
        .call<KeyCapturePayload>(
          'settings.captureKey',
          opts.requestFields(target),
          { timeoutMs: 0 },
        )
        .then((payload) => settle(target, payload))
        .catch((error: unknown) => {
          if (capturingRef.current !== target) return;
          settle(target, { cancelled: true });
          optionsRef.current.onError?.(error, target);
        });
      return;
    }

    const onKey = (event: KeyboardEvent) => {
      event.preventDefault();
      const name = domKeyName(event);
      const cancelled = event.key === 'Escape' || !name;
      settle(target, {
        name,
        cancelled,
        conflicts: cancelled
          ? []
          : optionsRef.current.standaloneConflicts?.(target, name) || [],
      });
    };
    window.addEventListener('keydown', onKey, true);
    domCleanupRef.current = () => window.removeEventListener('keydown', onKey, true);
  };

  return { capturing, isCapturing: () => capturingRef.current !== null, begin, finish };
}