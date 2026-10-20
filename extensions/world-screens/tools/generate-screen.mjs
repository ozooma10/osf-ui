import { mkdir, writeFile, access } from 'node:fs/promises';
import { resolve, dirname, join } from 'node:path';
import { worldTexturePath, worldPlaceholder } from './world-texture.mjs';
import { screenAssets } from './screen-assets.mjs';

const [id, output, profile = 'display-screen5', ...extra] = process.argv.slice(2);
if (!id || !output || extra.length || !/^[a-z0-9][a-z0-9._~-]{0,63}\/[a-z0-9-]{1,64}$/.test(id) ||
    /^(con|prn|aux|nul|com[1-9]|lpt[1-9])(?:\.|\/)/i.test(id) || id.split('/')[0].endsWith('.') ||
    id.startsWith('osfui/')) {
  throw new Error('Usage: node generate-screen.mjs <mod/view> <Data-directory> [display-screen5|full-uv]. Use lowercase IDs.');
}
const texture = worldTexturePath(id);
const files = screenAssets(id, profile);
files.set(texture, worldPlaceholder());
files.set(`SFSE/Plugins/OSF/UI/views/${id}/manifest.json`, Buffer.from(JSON.stringify({
  manifestVersion: 1, kind: 'world', width: 1280, height: 720, texture,
  capturesInput: false, pausesGame: false, entry: 'index.html'
}, null, 2) + '\n'));
const root = resolve(output);
// Check the entire output set before writing; never replace authored content.
for (const name of files.keys()) {
  try { await access(join(root, name)); } catch (error) { if (error.code === 'ENOENT') continue; throw error; }
  throw new Error(`Output already exists: ${join(root, name)}`);
}
for (const [name, bytes] of files) {
  const destination = join(root, name);
  await mkdir(dirname(destination), { recursive: true });
  await writeFile(destination, bytes, { flag: 'wx' });
}
console.log(`Generated ${files.size} files under ${root}; add your page, screen catalog, and content assets.`);
