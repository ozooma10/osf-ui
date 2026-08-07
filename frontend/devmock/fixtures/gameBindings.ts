// The game's own bindings, dev harness only.
//
// Native now copies the engine's live ControlMap and injects "@game"
// pseudo-entries. This sample exercises the
// "Starfield (…)" side of conflict badges and of the live-warn during a capture.

import type { KeybindingsData } from '@sdk';

/** Compact internal rows used only by the mock conflict engine. */
export interface GameBindingFixture {
  name: string;
  event: string;
  title: string;
}

export const GAME_BINDINGS: GameBindingFixture[] = [
  { name: 'F5', event: 'QuickSave', title: 'Starfield (Quicksave)' },
  { name: 'F9', event: 'QuickLoad', title: 'Starfield (Quickload)' },
  { name: 'E', event: 'Activate', title: 'Starfield (Interact)' },
  { name: 'Space', event: 'Jump', title: 'Starfield (Jump)' },
  { name: 'Grave', event: 'Console', title: 'Starfield (Console)' },
];

export const LIVE_KEYBINDINGS: KeybindingsData = {
  available: true,
  revision: 1,
  gameVersion: 'dev-harness',
  actions: GAME_BINDINGS.map((row, index) => ({
    event: row.event,
    label: row.title.replace(/^Starfield \((.*)\)$/, '$1'),
    category: 'MainGameplay',
    context: { id: 0, name: 'MainGameplay', order: 0 },
    classification: 'core',
    modes: { definite: ['onFoot', 'ship'], possible: ['vehicle', 'zeroG'] },
    sortIndex: index,
    required: false,
    bindings: [{ slot: 'main', key: row.name, chord: [row.name], unbound: false }],
  })),
};
