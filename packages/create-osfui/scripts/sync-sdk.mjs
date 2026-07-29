import { copyFile, mkdir } from 'node:fs/promises';
import { dirname, resolve } from 'node:path';
import { fileURLToPath } from 'node:url';

const here = dirname(fileURLToPath(import.meta.url));
const templateRoot = resolve(here, '..', 'templates', 'native');
const sdkRoot = resolve(here, '..', '..', '..', 'sdk');
await mkdir(templateRoot, { recursive: true });
for (const name of ['OSFUI_API.h', 'OSFUI_JSON.h']) {
  await copyFile(resolve(sdkRoot, name), resolve(templateRoot, name));
}
