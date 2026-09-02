import { readFileSync } from 'node:fs';
import { dirname, join } from 'node:path';
import { fileURLToPath } from 'node:url';

const HERE = dirname(fileURLToPath(import.meta.url));
export const CORE_HELPER = join(HERE, '..', 'src', 'shared-kit', 'osfui.js');
/** Deterministic 2.0 helper. There is intentionally no 1.x compatibility facade. */
export function composeHelper() {
  const core = readFileSync(CORE_HELPER, 'utf8').replace(/\s*$/, '');
  return `${core}\n`;
}
