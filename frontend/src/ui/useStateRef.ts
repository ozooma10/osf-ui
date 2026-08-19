import { useRef, useState } from 'preact/hooks';

export function useStateRef<T>(initial: T) {
  const [value, setValue] = useState<T>(initial);
  const ref = useRef<T>(initial);
  const set = (next: T) => {
    ref.current = next;
    setValue(next);
  };
  return [value, set, ref] as const;
}

export function useLatest<T>(value: T) {
  const ref = useRef<T>(value);
  ref.current = value;
  return ref;
}
