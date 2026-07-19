// KeyField.tsx — the `type:"key"` rebind button, plus its optional unbind ✕.
//
// Ports `buildKey` (settings/main.legacy.js:500-523).
//
// WHY THE CAPTURE IS NATIVE, NOT A keydown LISTENER: pressing the CURRENT
// overlay toggle key must rebind it, not close the overlay. Only the runtime
// sees the press before its own hotkey dispatch does, so clicking this arms
// `settings.captureKey` and the answer comes back as `settings.captured`. The
// browser-side fallback exists purely for the bridge-less preview.
//
// TWO padnav contracts live here:
//
//  * `class="listening"` while armed. src/legacy/padnav.js:207 suspends ALL
//    arrow navigation while any `.listening` element exists — the next key
//    press belongs to the capture. The class is appended AFTER the kit classes,
//    matching legacy's `classList.add` (so the className reads
//    "osf-btn osf-btn--sm osf-key listening").
//  * The ✕ is a real <button>, so padnav can reach it.
//
// The unbind affordance appears ONLY when BOTH hold: the schema opted into the
// unbound state (`allowUnbound`) AND there is a current value to clear
// (main.legacy.js:508). Without allowUnbound the store refuses "" outright, so
// offering the button would render a control whose only action is rejected.

export interface KeyFieldProps {
  id: string;
  /** The bound key name, or "" / undefined when unbound. */
  value: string | undefined;
  /** Schema `allowUnbound` — strictly, the ✕'s first precondition. */
  allowUnbound: boolean;
  /** True while THIS field's capture is armed. */
  listening: boolean;
  disabled: boolean;
  /** Arm the capture. */
  onRebind: () => void;
  /** Commit "" (the deliberate unbound state). */
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
      class={listening ? 'osf-btn osf-btn--sm osf-key listening' : 'osf-btn osf-btn--sm osf-key'}
      id={id}
      disabled={disabled}
      onClick={props.onRebind}
    >
      {/* An em-dash placeholder for "unbound" — legacy's `current || "—"`, so
          an empty-string value shows the dash rather than an empty button. */}
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
