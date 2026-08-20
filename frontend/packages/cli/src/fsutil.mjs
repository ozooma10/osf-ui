import { access } from 'node:fs/promises';

/** True when `path` names an existing filesystem entry. Falsy paths are absent. */
export async function exists(path) {
  if (!path) return false;
  return access(path).then(() => true, () => false);
}
