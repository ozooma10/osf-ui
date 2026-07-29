import { readFile } from 'node:fs/promises';
import { dirname, relative, resolve } from 'node:path';
import { fileURLToPath } from 'node:url';

const REGISTRY_SPEC = '^0.1.0';
const LOCAL_CLI_ROOT = resolve(dirname(fileURLToPath(import.meta.url)), '..', '..', 'cli');

export async function resolveCliSpec(projectRoot, override, localCliRoot = LOCAL_CLI_ROOT) {
  if (override) return override;
  try {
    const packageJson = JSON.parse(await readFile(resolve(localCliRoot, 'package.json'), 'utf8'));
    if (packageJson.name !== '@osfui/cli') return REGISTRY_SPEC;
    const localPath = relative(projectRoot, localCliRoot).replaceAll('\\', '/');
    return `file:${localPath || '.'}`;
  } catch {
    return REGISTRY_SPEC;
  }
}
