// The game's own bindings, dev harness only.
//
// Per mcm-design §9, native loads vanillakeys.json plus the engine's controlmap
// overrides and injects "@game" pseudo-entries. This sample exercises the
// "Starfield (…)" side of conflict badges and of the live-warn during a capture.

import type { SettingsDataPayload } from '@sdk';

/** `vanillaKeys` as it appears in a `settings.data` payload. */
export type VanillaKey = NonNullable<SettingsDataPayload['vanillaKeys']>[number];

export const VANILLA_KEYS: VanillaKey[] = [
  { name: 'F5', event: 'QuickSave', title: 'Starfield (Quicksave)' },
  { name: 'F9', event: 'QuickLoad', title: 'Starfield (Quickload)' },
  { name: 'E', event: 'Activate', title: 'Starfield (Interact)' },
  { name: 'Space', event: 'Jump', title: 'Starfield (Jump)' },
  { name: 'Grave', event: 'Console', title: 'Starfield (Console)' },
];
