export const CLI_VERSION = '0.1.0';
export const OSFUI_RELEASE_VERSION = '2.0.0';
export const BRIDGE_VERSION = '2.0';
export const CONFIG_FILES = ['osfui.config.ts', 'osfui.config.js', 'osfui.config.mjs'];
export const LOCAL_FILE = '.osfui/local.json';
export const AUTHOR_MARKER = '.author-mode.json';
// Written into outDir by `osfui build` so a later build can prove the
// directory is its own output before rm -rf'ing it. Excluded from packages
// and deploys.
export const BUILD_MARKER = '.osfui-build.json';

export const MAX_MOD_ID_LENGTH = 64;
export const MOD_ID_PATTERN = /^(?!.*[\u0000-\u001f<>:"/\\|?*#%])(?!.*[. ]$)(?!\.{1,2}$).+$/u;
export const VIEW_ID_PATTERN = /^[a-z0-9-]+$/;

const WINDOWS_DEVICE_ID = /^(?:con|prn|aux|nul|com[1-9]|lpt[1-9])(?:\.|$)/i;

export function isAcceptedModId(value) {
  if (typeof value !== 'string' || new TextEncoder().encode(value).byteLength > MAX_MOD_ID_LENGTH ||
      !MOD_ID_PATTERN.test(value)) {
    return false;
  }
  const lower = value.toLowerCase();
  return lower === 'osfui' ? value === 'osfui' : !WINDOWS_DEVICE_ID.test(value);
}
