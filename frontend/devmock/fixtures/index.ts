// fixtures/index.ts — one import site for the harness's demo datasets.
// DEV ONLY: nothing under devmock/ is ever part of a shipped view bundle.

export { FALLBACK_SCHEMAS } from './schemas';
export { MOCK_VIEWS, MOD_ASSET_ROOTS, HARNESS_PAGES, type MockView } from './views';
export { GAME_BINDINGS, LIVE_KEYBINDINGS, type GameBindingFixture } from './gameBindings';
