export const CLI_VERSION = '0.1.0';
export const OSFUI_RELEASE_VERSION = '2.0.0';
/** @deprecated Compatibility name. Use OSFUI_RELEASE_VERSION. */
export const HOST_VERSION = OSFUI_RELEASE_VERSION;
export const BRIDGE_VERSION = '2.0';
export const CONFIG_FILES = ['osfui.config.ts', 'osfui.config.js', 'osfui.config.mjs'];
export const LOCAL_FILE = '.osfui/local.json';
export const AUTHOR_MARKER = '.author-mode.json';
// Written into outDir by `osfui build` so a later build can prove the
// directory is its own output before rm -rf'ing it. Excluded from packages
// and deploys.
export const BUILD_MARKER = '.osfui-build.json';

export const MOD_ID_PATTERN = /^(?:osfui|[a-z0-9-]+\.[a-z0-9-]+)$/;
export const MAX_MOD_ID_LENGTH = 64;
export const VIEW_ID_PATTERN = /^[a-z0-9-]+$/;
