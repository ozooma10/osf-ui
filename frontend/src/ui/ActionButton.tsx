
import { useState } from 'preact/hooks';
import type { Translator } from '@lib/i18n';
import { codeOf } from '@lib/protocol';
import { cx } from './cx';

export const RESERVED_NS = ['ui', 'menu', 'hud', 'settings', 'views', 'game', 'runtime'] as const;

export const ACTION_TIMEOUT_MS = 5000;

export type ActionRefusal =
  | { kind: 'namespace'; endpoint: string }
  | { kind: 'reserved'; namespace: string }
  | null;

export function actionRefusal(modId: string, requestEndpoint: unknown): ActionRefusal {
  if (typeof requestEndpoint !== 'string' || !requestEndpoint.startsWith(modId + '.')) {
    return { kind: 'namespace', endpoint: String(requestEndpoint) };
  }
  const ns = requestEndpoint.slice(0, requestEndpoint.indexOf('.'));
  if ((RESERVED_NS as readonly string[]).includes(ns)) return { kind: 'reserved', namespace: ns };
  return null;
}

export interface ActionButtonProps {
  modId: string;
  /** Schema fields this control reads. `command` is the frozen request-endpoint field. */
  item: { key?: string; label?: string; command?: unknown; style?: string; confirm?: string };
  /** Result of `enabledWhen`; true when the schema declares no gate. */
  enabled: boolean;
  tr: Translator;
  onToast: (message: string, kind?: 'warn' | 'danger') => void;
  onRun: () => Promise<string | null>;
}

export function ActionButton({ modId, item, enabled, tr, onToast, onRun }: ActionButtonProps) {
  const [pending, setPending] = useState(false);
  const [confirming, setConfirming] = useState(false);

  const style =
    item.style === 'accent'
      ? ' osf-btn--osf-accent'
      : item.style === 'danger'
        ? ' osf-btn--danger'
        : '';

  const fire = () => {
    const refusal = actionRefusal(modId, item.command);
    if (refusal) {
      onToast(
        refusal.kind === 'namespace'
          ? tr('actionWrongNamespace', 'Action refused: {endpoint} is not namespaced to {mod}', {
              endpoint: `"${refusal.endpoint}"`,
              // Compatibility placeholder for existing translation catalogs.
              command: `"${refusal.endpoint}"`,
              mod: modId,
            })
          : tr('actionReserved', 'Action refused: {namespace} is a reserved framework namespace', {
              namespace: `"${refusal.namespace}."`,
            }),
        'danger',
      );
      return;
    }

    setPending(true);
    onRun().then(
      (message) => {
        setPending(false);
        if (message) onToast(message);
      },
      (err: unknown) => {
        setPending(false);
        const e = err as { message?: unknown } | null;
        const code = codeOf(err);
        if (code === 'timeout') {
          onToast(tr('noResponseFrom', 'No response from {mod}', { mod: modId }), 'warn');
          return;
        }
        const message = e && typeof e.message === 'string' && e.message ? e.message : '';
        onToast(message || tr('actionFailed', 'Action failed'), 'danger');
      },
    );
  };

  if (confirming) {
    return (
      <div class="confirm">
        <span class="confirm-msg">{item.confirm}</span>
        <button
          type="button"
          class="osf-btn osf-btn--sm osf-btn--danger"
          onClick={() => {
            setConfirming(false);
            fire();
          }}
        >
          {tr('confirm', 'Confirm')}
        </button>
        <button
          type="button"
          class="osf-btn osf-btn--sm osf-btn--ghost"
          onClick={() => setConfirming(false)}
        >
          {tr('cancel', 'Cancel')}
        </button>
      </div>
    );
  }

  return (
    <button
      type="button"
      class={cx('osf-btn', `osf-btn--sm${style}`, pending && 'pending')}
      disabled={pending || !enabled}
      onClick={() => (item.confirm ? setConfirming(true) : fire())}
    >
      {/* Resting label falls back to "Run" only when the schema names none. */}
      {pending ? '…' : item.label || tr('run', 'Run')}
    </button>
  );
}
