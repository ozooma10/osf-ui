// Mock view catalog (menus + HUDs in Mod Settings); dev only. Mirrors the
// OSF UI runtime's `osfui/views` state.
//
// Real shipped views come first: `menu.open` on one of those navigates the
// harness to it, so view launch works here the way it does in game. The
// `fixture: true` entries are fictional and exercise every state the Mod Settings
// view catalog renders — a view owned by a settings mod (`mod` matches a schema id),
// view-only mods with no schema, a failed load, HUD live / hidden. Hidden by
// default; toggled with the toolbar "Sample views" button or ?fixtures=1.
//
// View ids are qualified "<modId>/<viewName>" (api-freeze-plan item 1),
// mirroring the nested views/<modId>/<viewName>/ layout.

import type { ViewsData } from '@sdk';

/** One catalog entry, plus the harness-only "is this fictional?" marker. */
export type MockView = ViewsData['views'][number] & { fixture?: boolean };

export const MOD_ASSET_ROOTS: Record<string, string> = {};

// `targetVersion` is spelled on every entry ("" = undeclared) because the SDK
// marks the field required, so the send path never has to patch the shape.
export const MOCK_VIEWS: MockView[] = [
  {
    id: 'osfui/settings',
    title: 'Mod Settings',
    description: 'Installed mods — settings, menus, and HUD toggles.',
    mod: 'osfui',
    kind: 'menu',
    interactive: true,
    hub: false,
    targetVersion: '',
    open: false,
    focused: false,
    loadState: 'loaded',
    autoStart: true,
    autoStartMutable: false,
    pinned: true,
  },
  {
    id: 'osfui/keybinds',
    title: 'Keybindings',
    description: 'Full keyboard map of mod and game bindings.',
    mod: 'osfui',
    kind: 'menu',
    interactive: true,
    hub: true,
    targetVersion: '',
    open: false,
    focused: false,
    loadState: 'loaded',
    autoStart: false,
    autoStartMutable: false,
    pinned: false,
  },
  {
    id: 'acme.shipworks/almanac',
    title: 'Ship Almanac',
    description: 'Browse ship modules, mass and performance readouts.',
    mod: 'acme.shipworks',
    kind: 'menu',
    interactive: true,
    hub: true,
    targetVersion: '',
    open: false,
    focused: true,
    loadState: 'loaded',
    autoStart: false,
    autoStartMutable: false,
    pinned: false,
    fixture: true,
  },
  {
    id: 'acme.shipworks/hudwidgets',
    title: 'HUD Widgets',
    description: 'Clock and status HUDs shown during gameplay.',
    mod: 'acme.shipworks',
    kind: 'hud',
    interactive: false,
    hub: true,
    targetVersion: '',
    open: true,
    focused: false,
    loadState: 'loaded',
    autoStart: true,
    autoStartMutable: true,
    pinned: false,
    fixture: true,
  },
  // targetVersion newer than any real OSF UI — with fixtures on, the rail head
  // shows the "needs update" badge next to the target OSF UI release version.
  {
    id: 'acme.cargo/cargo',
    title: 'Cargo Manifest',
    description: 'Sortable inventory with a live mass budget.',
    mod: 'acme.cargo',
    kind: 'menu',
    interactive: true,
    hub: true,
    targetVersion: '99.0.0',
    open: false,
    focused: false,
    loadState: 'loaded',
    autoStart: false,
    autoStartMutable: false,
    pinned: false,
    fixture: true,
  },
  {
    id: 'acme.atlas/atlas',
    title: 'Star Atlas',
    description: 'Annotated survey routes and anomalies by system.',
    mod: 'acme.atlas',
    kind: 'menu',
    interactive: true,
    hub: true,
    targetVersion: '',
    open: false,
    focused: false,
    loadState: 'failed',
    autoStart: false,
    autoStartMutable: false,
    pinned: false,
    fixture: true,
  },
  {
    id: 'acme.vitals/vitals',
    title: 'Vitals Ring',
    description: 'O2, health and affliction indicators.',
    mod: 'acme.vitals',
    kind: 'hud',
    interactive: false,
    hub: true,
    targetVersion: '',
    open: false,
    focused: false,
    loadState: 'loaded',
    autoStart: false,
    autoStartMutable: true,
    pinned: false,
    fixture: true,
  },
  // Discovered on disk but never instantiated: a drop-in content view with no
  // schema and never opened. The launcher still lists it; opening it would
  // create its browser object and start document loading on demand in game.
  {
    id: 'acme.codex/codex',
    title: 'Field Codex',
    description: 'Reference Menu — dropped in, not yet opened.',
    mod: 'acme.codex',
    kind: 'menu',
    interactive: true,
    hub: true,
    targetVersion: '',
    open: false,
    focused: false,
    loadState: 'unloaded',
    autoStart: false,
    autoStartMutable: false,
    pinned: false,
    fixture: true,
  },
];

// The mock runs inside the view iframe under `osfui dev`, so a real
// `menu.open` navigates the iframe to the target view's own page.
export const HARNESS_PAGES: Record<string, string> = {
  'osfui/settings': '/osfui/settings/index.html',
  'osfui/keybinds': '/osfui/keybinds/index.html',
};
