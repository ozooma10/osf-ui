// The always-warm first-load handoff surface.
//
// Runtime.cpp preloads this view at startup and keeps it resident, then pushes
// `handoff.state` while a target view's renderer spins up. It is platform-
// private: no manifest schema, no i18n catalog of its own — the `title` field
// arrives already localised by native (Localization::Resolve), and the fixed
// chrome copy below is authored English, exactly as the pre-port script had it.
//
// Three contracts this component must not drift from:
//  - The element ids (#title, #owner, #target, #channel, #status, #detail,
//    #actions, #retry, #close) — style.css selects on them.
//  - `document.body`'s data-phase / data-live attributes, which style.css uses
//    to drive the phase colouring and the carrier animation. They live on body,
//    outside the Preact root, so they are applied as an effect.
//  - data-live is only set once a state has actually arrived. The pre-state
//    render is the "cold" look, and the first push is what lights it up.

import { useEffect, useLayoutEffect, useRef, useState } from 'preact/hooks';
import { windowBridge, type Bridge } from '@lib/bridge';
import type { HandoffStatePayload } from '@lib/protocol';

type Phase = HandoffStatePayload['phase'];

interface PhaseCopy {
  status: string;
  detail: string;
}

const COPY: Record<Phase, PhaseCopy> = {
  linking: {
    status: 'ESTABLISHING LOCAL LINK',
    detail: 'Negotiating display surface and local data channels.',
  },
  retrying: {
    status: 'SIGNAL INTERRUPTED // REACQUIRING',
    detail: 'The interface carrier dropped. Automatic relink is in progress.',
  },
  error: {
    status: 'LINK FAILED // INTERFACE UNAVAILABLE',
    detail: 'The local endpoint did not answer. Retry the link or return to the world.',
  },
};

/** Carrier bars. Count is cosmetic but style.css animates them by :nth-child. */
const CARRIER_BARS = 8;

/**
 * Two-digit channel tag derived from the target id — decoration, not identity.
 * Position-weighted so "a/b" and "b/a" differ; mod 100 to stay two digits.
 */
export function checksum(value: string): string {
  let n = 0;
  for (let i = 0; i < value.length; ++i) n = (n + value.charCodeAt(i) * (i + 1)) % 100;
  return String(n).padStart(2, '0');
}

/**
 * `phase` crosses the bridge as untrusted JSON, so an unknown value falls back
 * to "linking" rather than indexing COPY to undefined and blanking the panel.
 */
function phaseOf(state: HandoffStatePayload | null): Phase {
  const p = state?.phase;
  return p !== undefined && Object.prototype.hasOwnProperty.call(COPY, p) ? p : 'linking';
}

export interface AppProps {
  /**
   * Optional so the dev harness can mount `<App />` with no props — it renders
   * every view through one generic `FunctionComponent` slot. Production passes
   * `windowBridge` explicitly from main.tsx; tests pass a fake.
   */
  bridge?: Bridge;
}

export function App({ bridge = windowBridge }: AppProps) {
  const [state, setState] = useState<HandoffStatePayload | null>(null);
  const retryRef = useRef<HTMLButtonElement>(null);

  useEffect(() => bridge.on('handoff.state', (payload) => setState(payload)), [bridge]);

  // Escape closes from anywhere on the page, including when nothing is focused.
  useEffect(() => {
    const onKeyDown = (event: KeyboardEvent) => {
      if (event.key === 'Escape') bridge.send('close');
    };
    window.addEventListener('keydown', onKeyDown);
    return () => window.removeEventListener('keydown', onKeyDown);
  }, [bridge]);

  const phase = phaseOf(state);
  const copy = COPY[phase];
  const retry = !!state?.retry;

  // Effects, not render output: body and documentElement sit above the mount
  // point. Re-runs on every push (not just on change) so a repeated error state
  // re-focuses Retry, matching the pre-port behaviour.
  useLayoutEffect(() => {
    if (!state) return;
    document.body.dataset['phase'] = phase;
    document.body.dataset['live'] = 'true';
    bridge.applyAccent(document.documentElement, state.accent);
    if (state.retry) retryRef.current?.focus();
  }, [state, phase, bridge]);

  const title = String(state?.title || 'INTERFACE').toUpperCase();
  // Mod ids are dotted/underscored slugs; spacing them out reads as a name.
  const owner = String(state?.mod || 'LOCAL SYSTEM')
    .replace(/[._-]+/g, ' ')
    .toUpperCase();
  const target = String(state?.target || 'UNRESOLVED').toUpperCase();

  return (
    <main class="handoff" aria-live="polite">
      <section class="link-panel">
        <div class="edge edge--top" />
        <header>
          <span class="eyebrow">LOCAL INTERFACE // HANDOFF</span>
          <span class="channel" id="channel">
            OSF-LINK {checksum(String(state?.target || ''))}
          </span>
        </header>
        <div class="title-row">
          <span class="locator" aria-hidden="true" />
          <div>
            <div class="owner" id="owner">
              {owner}
            </div>
            <h1 id="title">{title}</h1>
          </div>
        </div>
        <div class="telemetry">
          <span class="status-light" aria-hidden="true" />
          <span id="status">{copy.status}</span>
          <div class="carrier" aria-hidden="true">
            {Array.from({ length: CARRIER_BARS }, (_, i) => (
              <i key={i} />
            ))}
          </div>
        </div>
        <p class="detail" id="detail">
          {copy.detail}
        </p>
        <div class="actions" id="actions" hidden={!retry}>
          <button
            class="osf-btn osf-btn--osf-accent"
            id="retry"
            type="button"
            ref={retryRef}
            onClick={() => bridge.send('osfui.handoffRetry')}
          >
            RETRY LINK
          </button>
          <button
            class="osf-btn osf-btn--ghost"
            id="close"
            type="button"
            onClick={() => bridge.send('close')}
          >
            CANCEL
          </button>
        </div>
        <footer>
          <span>LOCAL BUS</span>
          <span id="target">{target}</span>
        </footer>
        <div class="edge edge--bottom" />
      </section>
    </main>
  );
}
