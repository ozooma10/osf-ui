// fixtures/vanillaKeys.ts — the game's own bindings. DEV ONLY.
//
// mcm-design §9 "vanilla hotkeys": native loads vanillakeys.json plus the
// engine's controlmap overrides and injects "@game" pseudo-entries. The mock
// ships this small sample so the harness exercises the "Starfield (…)" side of
// conflict badges and of the live-warn during a key capture.
//
// Ported verbatim from devtools/harness/mockbridge.js:361-367.

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
