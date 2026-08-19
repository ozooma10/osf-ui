import { spawn } from 'node:child_process';
import { dirname, resolve } from 'node:path';
import { fileURLToPath } from 'node:url';

const projectRoot = resolve(dirname(fileURLToPath(import.meta.url)), '..');

const env = { ...process.env };
delete env.XSE_SF_MODS_PATH;
delete env.XSE_SF_GAME_PATH;

const code = await new Promise((resolve, reject) => {
  const child = spawn('xmake', ['build', '-P', projectRoot], { env, stdio: 'inherit' });
  child.once('error', reject);
  child.once('exit', resolve);
});
if (code !== 0) process.exit(code ?? 1);
