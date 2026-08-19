import { useEffect, useRef, useState } from 'preact/hooks';

export const OPEN_COOLDOWN_MS = 1600;

/**
 * Blocks duplicate launch clicks while a view handoff is in flight.
 * The timer is cancelled on unmount so a Settings hide/show reset cannot
 * update a stale launch control.
 */
export function useOpenCooldown(): { active: boolean; begin: () => boolean } {
  const [active, setActive] = useState(false);
  const activeRef = useRef(false);
  const timerRef = useRef<ReturnType<typeof setTimeout> | null>(null);

  useEffect(
    () => () => {
      if (timerRef.current !== null) clearTimeout(timerRef.current);
    },
    [],
  );

  const begin = (): boolean => {
    if (activeRef.current) return false;
    activeRef.current = true;
    setActive(true);
    timerRef.current = setTimeout(() => {
      timerRef.current = null;
      activeRef.current = false;
      setActive(false);
    }, OPEN_COOLDOWN_MS);
    return true;
  };

  return { active, begin };
}
