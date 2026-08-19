import { useEffect, useRef, useState } from 'preact/hooks';

export function useCommittedText(committed: string) {
  const [text, setText] = useState(committed);

  const lastCommitted = useRef(committed);
  useEffect(() => {
    if (lastCommitted.current !== committed) {
      lastCommitted.current = committed;
      setText(committed);
    }
  }, [committed]);

  return [text, setText] as const;
}
