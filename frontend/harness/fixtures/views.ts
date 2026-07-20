// Mock view catalog (panels + HUDs on the Mods surface); dev only. Mirrors the
// runtime's `views.data` push.
//
// Real shipped views come first: `menu.open` on one of those navigates the
// harness to it, so panel launch works here the way it does in game. The
// `fixture: true` entries are fictional and exercise every state the Mods
// surface renders — a view owned by a settings mod (`mod` matches a schema id),
// view-only mods with no schema, a failed load, HUD live / hidden. Hidden by
// default; toggled with the toolbar "Sample views" button or ?fixtures=1.
//
// View ids are qualified "<modId>/<viewName>" (api-freeze-plan item 1),
// mirroring the nested views/<modId>/<viewName>/ layout.

import type { ViewsDataPayload } from '@sdk';

/** One catalog entry, plus the harness-only "is this fictional?" marker. */
export type MockView = ViewsDataPayload['views'][number] & { fixture?: boolean };

/**
 * Where a mod's real view folder lives, relative to the harness page. In game
 * every view mounts under one views/ root, so the settings view resolves schema
 * `icon`/`image` assets at ../../<modId>/<file>; from the harness that lands
 * nowhere. The asset resolver consults this map (root + "/<modId>/<file>")
 * before falling back to "../..".
 */
export const MOD_ASSET_ROOTS: Record<string, string> = {
  'osf.animation': '../../../OSF Animation/views',
};

// `targetVersion` is spelled on every entry ("" = undeclared) because the SDK
// marks the field required, so the send path never has to patch the shape.
export const MOCK_VIEWS: MockView[] = [
  {
    id: 'osfui/settings',
    title: 'Mods',
    description: 'Installed mods — settings, panels and HUD toggles.',
    mod: 'osfui',
    kind: 'menu',
    interactive: true,
    hub: false,
    targetVersion: '',
    open: false,
    focused: false,
    loadState: 'loaded',
  },
  {
    id: 'osfui/keybinds',
    title: 'Keybinds',
    description: 'Full keyboard map of mod and game bindings.',
    mod: 'osfui',
    kind: 'menu',
    interactive: true,
    hub: true,
    targetVersion: '',
    open: false,
    focused: false,
    loadState: 'loaded',
  },
  // Real view from the sibling repo (VFS-merged in game). mod "osf.animation"
  // groups it onto the OSF Animation settings page (schema registered
  // natively). Open lands on the standalone osf.html page.
  {
    id: 'osf.animation/browser',
    title: 'OSF Animation Browser',
    description: 'Scene browser and launcher — crew, furniture, launch.',
    mod: 'osf.animation',
    kind: 'menu',
    interactive: true,
    hub: true,
    targetVersion: '',
    open: false,
    focused: false,
    loadState: 'loaded',
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
    fixture: true,
  },
  {
    id: 'acme.shipworks/hudwidgets',
    title: 'HUD Widgets',
    description: 'Clock and status overlays over the live game.',
    mod: 'acme.shipworks',
    kind: 'hud',
    interactive: false,
    hub: true,
    targetVersion: '',
    open: true,
    focused: false,
    loadState: 'loaded',
    fixture: true,
  },
  // targetVersion newer than any real OSF UI — with fixtures on, the rail head
  // shows the "needs update" badge next to the version number.
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
    fixture: true,
  },
];

/**
 * Where `menu.open` on a real shipped view lands. The harness is a single page
 * that swaps the mounted App off `?view=`. OSF Animation has its own page: it
 * loads the sibling repo's real view in an iframe and installs no mock bridge,
 * since that view self-mocks.
 */
export const HARNESS_PAGES: Record<string, string> = {
  'osfui/settings': '?view=osfui%2Fsettings',
  'osfui/keybinds': '?view=osfui%2Fkeybinds',
  'osf.animation/browser': 'osf.html',
};
