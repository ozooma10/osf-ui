// Switch.tsx — the kit's on/off control (`type:"bool"`, and the HUD toggles).
//
// Ports `buildBool` (settings/main.legacy.js:341-351). Three details are
// load-bearing:
//
//  1. It is a <button role="switch">, NOT a checkbox. osfui.css styles
//     `.osf-switch[aria-pressed="true"]`, so the STATE LIVES IN aria-pressed —
//     it is the visual, not just the accessible name. Swapping in an <input>
//     would render an unstyled box.
//  2. The initial state is `value === true`, STRICTLY. A missing value, or a
//     truthy non-boolean that slipped past the store, renders OFF rather than
//     guessing (main.legacy.js:345).
//  3. Legacy read the next state back off the DOM attribute
//     (`aria-pressed !== "true"`) rather than off the model. Deriving it from
//     `on` is equivalent because every commit applies optimistically to the
//     local model before the native ack — see `commit` in the settings App.

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
