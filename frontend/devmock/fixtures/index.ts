// fixtures/index.ts — one import site for the harness's demo datasets.
// DEV ONLY: nothing under harness/ is ever part of a shipped view bundle.

export { FALLBACK_SCHEMAS } from './schemas';
export { MOCK_VIEWS, MOD_ASSET_ROOTS, HARNESS_PAGES, type MockView } from './views';
export { VANILLA_KEYS, type VanillaKey } from './vanillaKeys';
export { MOCK_HEALTH, HEALTH_SCENARIOS, type MockHealth } from './diagnostics';
