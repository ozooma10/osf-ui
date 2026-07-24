// The `type:"key"` rebind button, plus its optional unbind ✕.
//
// Capture is native, not a keydown listener: pressing the current overlay
// toggle key must rebind it, not close the overlay, and only the runtime sees
// the press before its own hotkey dispatch. Clicking arms
// `settings.captureKey`; the answer comes back as `settings.captured`. The
// browser-side fallback exists only for the bridge-less preview.
//
// Two padnav contracts:
//  * `class="listening"` while armed — padnav suspends all arrow navigation
//    while any `.listening` element exists, since the next key press belongs
//    to the capture. That is a PRESENCE test (padnav.js:184), so only the class
//    itself is the padnav contract; its trailing POSITION is the kit's cx()
//    convention, pinned verbatim by dom-contracts.test.tsx ("osf-btn
//    osf-btn--sm osf-key listening"). An earlier version of this comment
//    presented the ordering as a padnav requirement — it is not.
//  * The ✕ is a real <button>, so padnav can reach it.
//
// The unbind affordance appears only when the schema opted into the unbound
// state (`allowUnbound`) and there is a current value to clear. Without
// allowUnbound the store refuses "", so the button's only action would be
// rejected.

import { cx } from './cx';

export interface KeyFieldProps {
  id: string;
  /** The bound key name, or "" / undefined when unbound. */
  value: string | undefined;
  allowUnbound: boolean;
  /** True while this field's capture is armed. */
  listening: boolean;
  disabled: boolean;
  onRebind: () => void;
  /** Commit "" (the unbound state). */
  onUnbind: () => void;
  /** Label shown while armed, e.g. tr("pressKey", "Press a key…"). */
  listeningLabel: string;
  /** `title` on the ✕. */
  unbindTitle: string;
  /** `aria-label` on the ✕ — names the setting, since "×" alone says nothing. */
  unbindLabel: string;
}

export function KeyField(props: KeyFieldProps) {
  const { id, value, allowUnbound, listening, disabled } = props;

  const button = (
    <button
      type="button"
      class={cx('osf-btn', 'osf-btn--sm', 'osf-key', listening && 'listening')}
      id={id}
      disabled={disabled}
      onClick={props.onRebind}
    >
      {/* Em-dash placeholder for "unbound", so an empty-string value shows the
          dash rather than an empty button. */}
      {listening ? props.listeningLabel : value || '—'}
    </button>
  );

  if (!allowUnbound || !value) return button;

  return (
    <span class="osf-key-wrap">
      {button}
      <button
        type="button"
        class="osf-btn osf-btn--sm osf-btn--ghost osf-key-clear"
        title={props.unbindTitle}
        aria-label={props.unbindLabel}
        disabled={disabled}
        onClick={props.onUnbind}
      >
        {/* U+00D7 MULTIPLICATION SIGN, the glyph the shipped view draws. */}
        ×
      </button>
    </span>
  );
}
