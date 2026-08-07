import { readFileSync } from 'node:fs';
import { dirname, join } from 'node:path';
import { fileURLToPath } from 'node:url';

const HERE = dirname(fileURLToPath(import.meta.url));
export const CORE_HELPER = join(HERE, '..', 'src', 'shared-kit', 'osfui.js');
export const V1_FACADE = join(HERE, '..', 'src', 'compat', 'v1', 'osfui-v1.js');

/** Deterministic shipped helper: untouched 2.0 source followed by guarded v1. */
export function composeHelper() {
  const core = readFileSync(CORE_HELPER, 'utf8').replace(/\s*$/, '');
  const compat = readFileSync(V1_FACADE, 'utf8').replace(/^\s*|\s*$/g, '');
  return `${core}\n\n${compat}\n`;
}
