// Control cell for a `type:"action"` row.
//
// A schema is untrusted author data and this button calls the request endpoint
// named by the compatibility field `command`, so two rules gate it client-side:
//
//   1. the endpoint must start with `<mod.id>.` — a mod may only call into its
//      own namespace;
//   2. the leading namespace must not be one of RESERVED_NS — framework endpoint
//      families, which no mod owns even if its schema claims one as its id.
//
// Rule 2 is not implied by rule 1: the store rejects reserved mod ids on load,
// but the renderer also runs against mock data and against whatever an older
// OSF UI runtime served. Native refuses both cases independently; this pair exists to
// give a message instead of a silent no-op, not to be the only check.
//
// The inline confirm is component state, not DOM-only, so a half-answered
// confirmation survives a full re-render of the pane (preset apply, external
// `settings.changed`).
//
// The `pending` class is a contract with the row's enabledWhen handling: the
// settings App must not re-enable a mid-flight action button underneath itself,
// so `disabled` here is `pending || !enabled`.

import { useState } from 'preact/hooks';
import type { Translator } from '@lib/i18n';
import { codeOf } from '@lib/protocol';
import { cx } from './cx';

/**
 * Request-endpoint namespaces owned by the framework. Mirrors the reserved-id
 * list in SettingsStore.cpp.
 */
export const RESERVED_NS = ['ui', 'menu', 'hud', 'settings', 'views', 'game', 'runtime'] as const;

export const ACTION_TIMEOUT_MS = 5000;

export type ActionRefusal =
  | { kind: 'namespace'; endpoint: string }
  | { kind: 'reserved'; namespace: string }
  | null;

/**
 * Why this request endpoint may not be called, or null when it may. The
 * `namespace` kind also covers a non-string compatibility `command` field.
 */
export function actionRefusal(modId: string, requestEndpoint: unknown): ActionRefusal {
  if (typeof requestEndpoint !== 'string' || !requestEndpoint.startsWith(modId + '.')) {
    return { kind: 'namespace', endpoint: String(requestEndpoint) };
  }
  // First dot only, so "acme.tools.run" has namespace "acme", not "acme.tools".
  // Same split native uses.
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
  /**
   * Call the request endpoint. Resolves with an optional message to display, rejects
   * with an error carrying `.code` ("timeout") / `.message`. The caller owns
   * the bridge and the ACTION_TIMEOUT_MS option.
   */
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
        // A plugin may answer with its own text; `ok:true` alone is silent.
        // No kind: no stylesheet defines "info" (see the ToastKind note in
        // @lib/toast).
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
      // `pending ||` first: a mid-flight button stays disabled even if its
      // enabledWhen gate has since flipped true.
      disabled={pending || !enabled}
      onClick={() => (item.confirm ? setConfirming(true) : fire())}
    >
      {/* Resting label falls back to "Run" only when the schema names none. */}
      {pending ? '…' : item.label || tr('run', 'Run')}
    </button>
  );
}
