// Root `npm run dev` launcher: ensures the frontend deps exist, then starts
// the authoring harness (`osfui dev` on the frontend project) — the same one
// third-party mods use — which opens the browser itself.
// Kept as a Node script so it works the same in PowerShell, cmd, and bash.
import { existsSync } from 'node:fs';
import { spawn, spawnSync } from 'node:child_process';
import { dirname, resolve } from 'node:path';
import { fileURLToPath } from 'node:url';

const here = dirname(fileURLToPath(import.meta.url));
const frontend = resolve(here, '..', 'frontend');
// `shell: true` is required so Node (v20+ on Windows) will run the `npm` shim
// (npm.cmd) without an EINVAL. We pass each command as a single string (not
// command + args array) to avoid the DEP0190 shell-escaping warning.
const opts = { cwd: frontend, stdio: 'inherit', shell: true };

if (!existsSync(resolve(frontend, 'node_modules'))) {
  console.log('[dev] frontend/node_modules missing — running `npm ci` (first run only)…');
  const install = spawnSync('npm ci', opts);
  if (install.status !== 0) process.exit(install.status ?? 1);
}

const dev = spawn('npm run dev', opts);
dev.on('exit', (code) => process.exit(code ?? 0));
