import { access, readFile } from 'node:fs/promises';
import { dirname, resolve } from 'node:path';
import { fileURLToPath } from 'node:url';

const HERE = dirname(fileURLToPath(import.meta.url));

/** Locate one shared-kit asset in the published package or source checkout. */
export async function sharedAssetPath(name) {
  const candidates = [
    resolve(HERE, '..', 'assets', name),
    resolve(HERE, '..', '..', '..', 'frontend', 'src', 'shared-kit', name),
  ];
  for (const path of candidates) {
    try {
      await access(path);
      return path;
    } catch (error) {
      if (error?.code !== 'ENOENT') throw error;
    }
  }
  throw new Error(`Missing packaged shared asset ${name}.`);
}

export async function readSharedAsset(name) {
  return readFile(await sharedAssetPath(name), 'utf8');
}
