
import type { SettingsData } from '@sdk';

export type KeyLabeler = (name: string) => string | undefined;

const none: KeyLabeler = () => undefined;

export function makeLabeler(keyboard: SettingsData['keyboard'] | null | undefined): KeyLabeler {
  const labels = keyboard && typeof keyboard === 'object' ? keyboard.labels : undefined;
  if (!labels || typeof labels !== 'object') return none;
  return (name) => {
    if (!Object.prototype.hasOwnProperty.call(labels, name)) return undefined;
    const label = (labels as Record<string, unknown>)[name];
    return typeof label === 'string' && label ? label : undefined;
  };
}
