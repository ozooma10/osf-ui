export const OSFUI_RELEASE_VERSION = '2.0.0';
export const MAX_MOD_ID_LENGTH = 64;

const MOD_ID_PATTERN = /^(?!.*[\u0000-\u001f<>:"/\\|?*#%])(?!.*[. ]$)(?!\.{1,2}$).+$/u;
const WINDOWS_DEVICE_ID = /^(?:con|prn|aux|nul|com[1-9]|lpt[1-9])(?:\.|$)/i;

export function isThirdPartyModId(value) {
  if (typeof value !== 'string' || new TextEncoder().encode(value).byteLength > MAX_MOD_ID_LENGTH ||
      !MOD_ID_PATTERN.test(value)) {
    return false;
  }
  return value.toLowerCase() !== 'osfui' && !WINDOWS_DEVICE_ID.test(value);
}
