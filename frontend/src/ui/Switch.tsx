
export interface SwitchProps {
  /** Omitted when the switch is not a labelled control's target. */
  id: string;
  /** Anything other than exactly `true` is off. */
  on: boolean;
  disabled: boolean;
  onToggle: (next: boolean) => void;
}

export function Switch({ id, on, disabled, onToggle }: SwitchProps) {
  return (
    <button
      type="button"
      class="osf-switch"
      role="switch"
      aria-checked={on ? 'true' : 'false'}
      aria-pressed={on ? 'true' : 'false'}
      disabled={disabled}
      {...(id ? { id } : {})}
      onClick={() => onToggle(!on)}
    />
  );
}
