import { dirname, resolve } from 'node:path';
import { fileURLToPath } from 'node:url';

const frontend = dirname(fileURLToPath(import.meta.url));

export const aliases = {
  '@sdk': resolve(frontend, '../sdk/osfui.d.ts'),
  '@lib': resolve(frontend, 'src/lib'),
  '@ui': resolve(frontend, 'src/ui'),
  '@views': resolve(frontend, 'src/views'),
};
