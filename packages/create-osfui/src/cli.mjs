#!/usr/bin/env node
import { mkdir, readdir, writeFile } from 'node:fs/promises';
import { basename, resolve } from 'node:path';
import { spawn } from 'node:child_process';
import {
  finishPrompt,
  ID,
  MOD_ID,
  promptMissing,
  PromptCancelledError,
  slug,
} from './prompts.mjs';

function parse(argv) {
  const result = { _: [] };
  for (let index = 0; index < argv.length; index++) {
    const value = argv[index];
    if (!value.startsWith('--')) result._.push(value);
    else {
      const key = value.slice(2).replace(/-([a-z])/g, (_, letter) => letter.toUpperCase());
      if (['--yes', '--no-install', '--help'].includes(value)) result[key] = true;
      else result[key] = argv[++index];
    }
  }
  return result;
}

function validate(options) {
  if (!MOD_ID.test(options.modId)) throw new Error('--mod-id must be lowercase <author>.<modname>.');
  if (!ID.test(options.view)) throw new Error('--view must use lowercase letters, digits, and hyphens.');
  if (!['preact', 'vanilla'].includes(options.template)) throw new Error('--template must be preact or vanilla.');
  if (!['menu', 'hud'].includes(options.surface)) throw new Error('--surface must be menu or hud.');
  if (!['papyrus', 'native', 'settings', 'static'].includes(options.integration)) {
    throw new Error('--integration must be papyrus, native, settings, or static.');
  }
}

async function put(root, relative, content) {
  const path = resolve(root, relative);
  await mkdir(resolve(path, '..'), { recursive: true });
  await writeFile(path, content);
}

function appSource(options) {
  const intro = {
    papyrus: 'Papyrus request',
    native: 'Native bridge command',
    settings: 'Settings-backed view',
    static: 'Static view',
  }[options.integration];
  const command = {
    papyrus: 'ui.papyrusRequest',
    native: `${options.modId}.example`,
    settings: 'settings.get',
    static: '',
  }[options.integration];
  const fields = options.integration === 'papyrus'
    ? `{ mod: '${options.modId}', request: 'example', args: [] }`
    : '{}';
  if (options.template === 'vanilla') return `import './style.css';
import '/shared/osfui.css';
import '/shared/osfui.js';

const app = document.querySelector('#app');
if (!(app instanceof HTMLElement)) throw new Error('Missing #app element');
app.innerHTML = '<main class="card"><p class="eyebrow">${options.surface.toUpperCase()} VIEW</p>' +
  '<h1>${intro}</h1><p>Edit and save to hot-reload.</p><button id="action">Test workflow</button>' +
  '<output id="status">Waiting for OSF UI…</output></main>';
const status = document.querySelector('#status');
const action = document.querySelector('#action');
if (!(status instanceof HTMLOutputElement) || !(action instanceof HTMLButtonElement)) {
  throw new Error('Template controls are missing');
}
window.osfui?.on?.('runtime.ready', (message) => {
  status.textContent = \`Connected to OSF UI \${message.payload.version}\`;
});
action.addEventListener('click', async () => {
  ${options.integration === 'static' ? "status.textContent = 'Static preset: no native bridge required'; return;" : ''}
  try {
    const request = window.osfui?.request;
    if (!request) throw new Error('OSF UI bridge is unavailable');
    const result = await request('${command}', ${fields});
    status.textContent = JSON.stringify(result.payload);
  } catch (error) {
    status.textContent = error instanceof Error ? error.message : String(error);
  }
});
`;
  return `import { render } from 'preact';
import { useState } from 'preact/hooks';
import './style.css';
import '/shared/osfui.css';
import '/shared/osfui.js';

function App() {
  const [status, setStatus] = useState('Waiting for OSF UI…');
  async function run() {
    ${options.integration === 'static' ? "setStatus('Static preset: no native bridge required'); return;" : ''}
    try {
      const request = window.osfui?.request;
      if (!request) throw new Error('OSF UI bridge is unavailable');
      const result = await request('${command}', ${fields});
      setStatus(JSON.stringify(result.payload));
    } catch (error) {
      setStatus(error instanceof Error ? error.message : String(error));
    }
  }
  return <main class="card">
    <p class="eyebrow">${options.surface.toUpperCase()} VIEW</p>
    <h1>${intro}</h1>
    <p>Edit this component and save. The harness updates without restarting Starfield.</p>
    <button onClick={run}>Test bridge</button>
    <output>{status}</output>
  </main>;
}

const root = document.querySelector('#app');
if (!root) throw new Error('Missing #app element');
render(<App />, root);
`;
}

async function scaffold(options) {
  const root = resolve(options.directory);
  await mkdir(root, { recursive: true });
  if ((await readdir(root)).length) throw new Error(`Directory is not empty: ${root}`);
  const viewRoot = `src/views/${options.modId}/${options.view}`;
  const extension = options.template === 'preact' ? 'tsx' : 'ts';
  const cliSpec = options.cliSpec || '^0.1.0';
  const packageJson = {
    name: slug(basename(root)),
    version: '0.1.0',
    private: true,
    type: 'module',
    scripts: {
      dev: 'osfui dev',
      'dev:game': 'osfui dev --game',
      check: 'osfui check',
      build: 'osfui build',
      package: 'osfui package',
      doctor: 'osfui doctor',
    },
    dependencies: options.template === 'preact' ? { preact: '^10.28.0' } : {},
    devDependencies: { '@osfui/cli': cliSpec },
  };
  await put(root, 'package.json', `${JSON.stringify(packageJson, null, 2)}\n`);
  await put(root, '.gitignore', 'node_modules/\ndist/\nrelease/\n.osfui/\n');
  await put(root, 'tsconfig.json', `${JSON.stringify({
    compilerOptions: {
      target: 'ES2022',
      module: 'ESNext',
      moduleResolution: 'Bundler',
      strict: true,
      jsx: 'react-jsx',
      jsxImportSource: 'preact',
      // DOM: the mock module runs in the browser (osfui check type-checks it).
      lib: ['ES2022', 'DOM', 'DOM.Iterable'],
      types: ['@osfui/cli/view'],
      noEmit: true,
    },
    include: ['src', 'osfui.config.ts', 'osfui.mock.ts'],
  }, null, 2)}\n`);
  await put(root, 'src/vite-env.d.ts', `declare module '*.css';
declare module '*osfui.js';
`);
  await put(root, 'osfui.config.ts', `import { defineConfig } from '@osfui/cli';

export default defineConfig({
  modId: '${options.modId}',
  views: [{
    id: '${options.view}',
    title: '${options.view.replaceAll('-', ' ')}',
    kind: '${options.surface}',
    width: ${options.surface === 'hud' ? 1920 : 1200},
    height: ${options.surface === 'hud' ? 1080 : 720},
    transparent: true,
    permissions: { nativeBridge: ${options.integration !== 'static'} },
  }],
});
`);
  await put(root, 'osfui.mock.ts', `import { defineMock } from '@osfui/cli';

// Browser-side mock served to \`osfui dev\`. Lives at the project root so it
// can never ship with the views. Request values may be plain JSON,
// { $type, payload } to control the reply type, or (async) functions of the
// command payload.
export default defineMock({
  state: { example: { enabled: true } },
  requests: {
    'papyrus.example': { ok: true, message: 'Mock Papyrus response' },
    '${options.modId}.example': { ok: true, message: 'Mock native-plugin response' },
    'settings.get': (payload) => ({ mods: [{ id: '${options.modId}', values: { example: true } }] }),
  },
  locales: { en: { title: 'Example view' } },
});

// Need full control (stateful round-trips, custom pushes)? Export install():
// export function install(ctx: MockContext) {
//   ctx.onCommand((command, payload, reply) => {
//     if (command !== '${options.modId}.ping') return;
//     reply('ui.result', { ok: true, at: Date.now() });
//     return true;
//   });
// }
`);
  await put(root, `${viewRoot}/index.html`, `<!doctype html>
<html lang="en"><head><meta charset="UTF-8"><meta name="viewport" content="width=device-width,initial-scale=1">
<title>${options.view}</title></head><body><div id="app"></div><script type="module" src="./main.${extension}"></script></body></html>
`);
  await put(root, `${viewRoot}/main.${extension}`, appSource(options));
  await put(root, `${viewRoot}/style.css`, `:root { font-family: system-ui, sans-serif; color: #eef7fb; background: transparent; }
body { margin: 0; min-height: 100vh; display: grid; place-items: center; }
.card { width: min(520px, 80vw); padding: 32px; background: rgba(8, 19, 27, .94); border: 1px solid #5aa8c7; }
.eyebrow { color: #7bdcff; letter-spacing: .18em; }
button { padding: 10px 16px; }
output { display: block; margin-top: 16px; color: #a9dced; }
`);
  await put(root, 'README.md', `# ${packageJson.name}

Run \`npm run dev\` for instant browser HMR. Run \`npm run dev:game -- --deploy "path-to-your-mod"\`
to sync into Starfield with temporary author mode, F11 reload, and F12 DevTools.

Use \`npm run package\` to create a release-ready zip.
`);
  return root;
}

function install(root) {
  return new Promise((resolvePromise, reject) => {
    const child = spawn('npm', ['install'], { cwd: root, stdio: 'inherit', shell: process.platform === 'win32' });
    child.once('exit', (code) => code === 0 ? resolvePromise() : reject(new Error(`npm install exited with ${code}`)));
  });
}

async function main() {
  const options = parse(process.argv.slice(2));
  if (options.help) {
    console.log('npm create osfui@latest [directory] [-- --mod-id author.mod --view main --template preact --surface menu --integration papyrus]');
    return;
  }
  options.directory = options._[0];
  const interactive = await promptMissing(options);
  validate(options);
  const root = await scaffold(options);
  if (!options.noInstall) await install(root);
  const next = root === process.cwd()
    ? 'npm run dev'
    : `cd ${options.directory}\n  npm run dev`;
  const result = `Created ${root}\n\nNext:\n  ${next}`;
  if (interactive) finishPrompt(result);
  else console.log(`\n${result}`);
}

main().catch((error) => {
  if (error instanceof PromptCancelledError) return;
  console.error(`create-osfui: ${error.message}`);
  process.exitCode = 1;
});
