// The kit's on/off control (`type:"bool"`, and the HUD toggles).
//
//  1. A <button role="switch">, not a checkbox. osfui.css styles
//     `.osf-switch[aria-pressed="true"]`, so aria-pressed carries the visual
//     state, not just the accessible name. An <input> renders an unstyled box.
//  2. State is `value === true` strictly: a missing value, or a truthy
//     non-boolean that slipped past the store, renders off rather than guessing.
//  3. Next state is derived from `on` rather than read back off the DOM
//     attribute, which is equivalent because every commit applies optimistically
//     to the local model before the native ack (see `commit` in the settings App).

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
      aria-pressed={on ? 'true' : 'false'}
      disabled={disabled}
      {...(id ? { id } : {})}
      onClick={() => onToggle(!on)}
    />
  );
}
