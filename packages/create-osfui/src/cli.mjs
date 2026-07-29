#!/usr/bin/env node
import { cp, mkdir, readdir, writeFile } from 'node:fs/promises';
import { basename, dirname, resolve } from 'node:path';
import { spawn } from 'node:child_process';
import { fileURLToPath } from 'node:url';
import {
  finishPrompt,
  ID,
  MOD_ID,
  promptMissing,
  PromptCancelledError,
  slug,
} from './prompts.mjs';
import { resolveCliSpec } from './cli-spec.mjs';
import { backendFiles, backendGuide } from './backend-templates.mjs';

const HERE = dirname(fileURLToPath(import.meta.url));

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
  if (!['typescript', 'javascript'].includes(options.template)) throw new Error('--template must be typescript or javascript.');
  if (!['menu', 'hud'].includes(options.surface)) throw new Error('--surface must be menu or hud.');
  if (!['papyrus', 'native'].includes(options.integration)) {
    throw new Error('--integration must be papyrus or native.');
  }
}

async function put(root, relative, content) {
  const path = resolve(root, relative);
  await mkdir(resolve(path, '..'), { recursive: true });
  await writeFile(path, content);
}

function nativeAppSource(options) {
  const typescript = options.template === 'typescript';
  const stateType = `${options.modId}.state`;
  const noticeType = `${options.modId}.notice`;
  const declarations = typescript ? `type DemoState = {
  count: number;
  enabled: boolean;
  greeting: string;
  lastAction: string;
  features: string[];
};
type Greeting = {
  message: string;
  receivedFromJs: { name: string; excited: boolean };
  nativeCount: number;
};
` : '';
  const requiredSignature = typescript
    ? '<T extends Element>(selector: string, kind: { new(): T }): T'
    : '(selector, kind)';
  const showStateSignature = typescript ? '(state: DemoState)' : '(state)';
  const stateTypeArgument = typescript ? '<DemoState>' : '';
  const noticeTypeArgument = typescript ? '<{ message: string }>' : '';
  const greetingTypeArgument = typescript ? '<Greeting>' : '';

  return `import './style.css';
import '/shared/osfui.css';
import '/shared/osfui.js';

${declarations}
const app = document.querySelector('#app');
if (!(app instanceof HTMLElement)) throw new Error('Missing #app element');
app.innerHTML = '<main class="card"><p class="eyebrow">${options.surface.toUpperCase()} · NATIVE BRIDGE</p>' +
  '<h1>C++ ↔ JavaScript</h1><p>One generated project showing commands, requests, pushes, settings, and callbacks.</p>' +
  '<section class="state"><span>Native count</span><strong id="count">—</strong>' +
  '<small id="last-action">Waiting for C++ state…</small><small id="features"></small></section>' +
  '<div class="actions"><button id="increment">Send command to C++</button></div>' +
  '<form id="greeting"><input id="name" value="Explorer" aria-label="Name">' +
  '<label><input id="excited" type="checkbox" checked> Enthusiastic</label>' +
  '<button type="submit">Call C++ and await reply</button></form>' +
  '<output id="status">Waiting for OSF UI…</output></main>';

function requiredElement${requiredSignature} {
  const element = document.querySelector(selector);
  if (!(element instanceof kind)) throw new Error('Missing ' + selector);
  return element;
}

const count = requiredElement('#count', HTMLElement);
const lastAction = requiredElement('#last-action', HTMLElement);
const features = requiredElement('#features', HTMLElement);
const increment = requiredElement('#increment', HTMLButtonElement);
const form = requiredElement('#greeting', HTMLFormElement);
const name = requiredElement('#name', HTMLInputElement);
const excited = requiredElement('#excited', HTMLInputElement);
const status = requiredElement('#status', HTMLOutputElement);

function showState${showStateSignature} {
  count.textContent = String(state.count);
  lastAction.textContent = state.lastAction;
  features.textContent = state.features.join(' · ');
  increment.disabled = !state.enabled;
}

// C++ -> JS: subscribe before asking for current state so later pushes cannot race us.
window.osfui?.on?.${stateTypeArgument}('${stateType}', showState);
window.osfui?.on?.${noticeTypeArgument}('${noticeType}', (payload) => {
  status.textContent = payload.message;
});

window.osfui?.ready?.then(async (info) => {
  status.textContent = 'Connected to OSF UI ' + info.version;
  try {
    if (!window.osfui?.call) throw new Error('Request API is unavailable');
    showState(await window.osfui.call${stateTypeArgument}('${options.modId}.getState'));
  } catch (error) {
    status.textContent = error instanceof Error ? error.message : String(error);
  }
});

// JS -> C++ fire-and-forget; OnIncrement answers by pushing ${stateType}.
increment.addEventListener('click', () => {
  if (!window.osfui?.send?.('${options.modId}.increment', { amount: 1 })) {
    status.textContent = 'OSF UI bridge is unavailable';
  }
});

// JS -> C++ request/response; OSF UI owns the request id and timeout.
form.addEventListener('submit', async (event) => {
  event.preventDefault();
  try {
    if (!window.osfui?.call) throw new Error('Request API is unavailable');
    const reply = await window.osfui.call${greetingTypeArgument}('${options.modId}.greet', {
      name: name.value,
      excited: excited.checked,
    });
    status.textContent = reply.message + ' Echo: ' + JSON.stringify(reply.receivedFromJs);
  } catch (error) {
    status.textContent = error instanceof Error ? error.message : String(error);
  }
});
`;
}

function mockSource(options) {
  const typescript = options.template === 'typescript';
  const mockImport = typescript
    ? "import { defineMock, type MockContext } from '@osfui/cli';"
    : "import { defineMock } from '@osfui/cli';";
  const contextType = typescript ? ': MockContext' : '';
  const messageType = typescript ? ': string' : '';
  if (options.integration === 'native') return `${mockImport}

// The browser harness mirrors native/src/main.cpp so every round trip works
// without launching Starfield. This file stays at project root and never ships.
const state = {
  count: 0,
  enabled: true,
  greeting: 'Hello from the mocked C++ plugin',
  lastAction: 'Browser mock initialized',
  features: ['typed JSON', 'commands', 'requests', 'native pushes', 'settings', 'hotkeys'],
};

export default defineMock({
  locales: { en: { title: 'Native bridge example' } },
});

export function install(ctx${contextType}) {
  const pushState = () => ctx.send({
    type: '${options.modId}.state',
    payload: { ...state, features: [...state.features] },
  });
  const notice = (message${messageType}) => ctx.send({
    type: '${options.modId}.notice', payload: { message },
  });

  ctx.onCommand((command, payload, reply) => {
    if (command === '${options.modId}.getState') {
      reply('${options.modId}.state', { ...state, features: [...state.features] });
      return true;
    }
    if (command === '${options.modId}.increment') {
      const requested = Number(payload.amount);
      const amount = Number.isFinite(requested) ? Math.max(-10, Math.min(10, requested)) : 1;
      if (state.enabled) {
        state.count += amount;
        state.lastAction = 'JavaScript sent a fire-and-forget command';
        pushState();
      } else {
        notice('The native counter is disabled in Mod Settings');
      }
      return true;
    }
    if (command === '${options.modId}.greet') {
      const name = typeof payload.name === 'string' ? payload.name : '';
      if (!name) {
        reply('ui.error', { code: 'invalid-payload', message: 'name is required' });
        return true;
      }
      const excited = payload.excited === true;
      reply('${options.modId}.greeting', {
        message: state.greeting + ', ' + name + (excited ? '!!' : '!'),
        receivedFromJs: { name, excited },
        nativeCount: state.count,
      });
      return true;
    }
  });

  ctx.registerTools([
    { id: 'native-enabled', kind: 'toggle', label: 'Native enabled', value: true },
    { id: 'native-hotkey', kind: 'button', label: 'Fire hotkey callback' },
  ], (id, value) => {
    if (id === 'native-enabled') {
      state.enabled = value === true;
      state.lastAction = 'Mocked C++ settings callback applied a value';
      pushState();
    } else if (id === 'native-hotkey') {
      state.lastAction = 'Mocked C++ hotkey callback fired';
      pushState();
      notice('The native open-view hotkey fired');
    }
  });
}
`;

  return `import { defineMock } from '@osfui/cli';

// Browser-side mock served to \`osfui dev\`. Lives at the project root so it
// can never ship with the views. Request values may be plain JSON,
// { $type, payload } to control the reply type, or (async) functions of the
// command payload.
export default defineMock({
  state: { example: { enabled: true } },
  requests: {
    'papyrus.example': { ok: true, message: 'Mock Papyrus response' },
  },
  locales: { en: { title: 'Example view' } },
});

// Need full control (stateful round-trips, custom pushes)? Export install():
// export function install(ctx) {
//   ctx.onCommand((command, payload, reply) => {
//     if (command !== '${options.modId}.ping') return;
//     reply('ui.result', { ok: true, at: Date.now() });
//     return true;
//   });
// }
`;
}

function appSource(options) {
  if (options.integration === 'native') return nativeAppSource(options);
  const intro = 'Papyrus request';
  const command = 'ui.papyrusRequest';
  const fields = `{ mod: '${options.modId}', request: 'example', args: [] }`;
  const readyTypeArgument = options.template === 'typescript' ? '<{ version: string }>' : '';
  const requiredSignature = options.template === 'typescript'
    ? '<T extends Element>(selector: string, kind: { new(): T }): T'
    : '(selector, kind)';

  return `import './style.css';
import '/shared/osfui.css';
import '/shared/osfui.js';

const app = document.querySelector('#app');
if (!(app instanceof HTMLElement)) throw new Error('Missing #app element');
app.innerHTML = '<main class="card"><p class="eyebrow">${options.surface.toUpperCase()} VIEW</p>' +
  '<h1>${intro}</h1><p>Edit and save to hot-reload.</p><button id="action">Test workflow</button>' +
  '<output id="status">Waiting for OSF UI…</output></main>';

function requiredElement${requiredSignature} {
  const element = document.querySelector(selector);
  if (!(element instanceof kind)) throw new Error('Missing ' + selector);
  return element;
}

const status = requiredElement('#status', HTMLOutputElement);
const action = requiredElement('#action', HTMLButtonElement);
window.osfui?.on?.${readyTypeArgument}('runtime.ready', (payload) => {
  status.textContent = 'Connected to OSF UI ' + payload.version;
});
action.addEventListener('click', async () => {
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
}

async function scaffold(options) {
  const root = resolve(options.directory);
  await mkdir(root, { recursive: true });
  if ((await readdir(root)).length) throw new Error(`Directory is not empty: ${root}`);
  const viewRoot = `src/views/${options.modId}/${options.view}`;
  const extension = options.template === 'typescript' ? 'ts' : 'js';
  const cliSpec = await resolveCliSpec(root, options.cliSpec);
  const scripts = {
    dev: 'osfui dev',
    'dev:game': 'osfui dev --game',
    check: 'osfui check',
    build: 'osfui build',
    package: 'osfui package',
    doctor: 'osfui doctor',
  };
  if (options.integration === 'native') {
    scripts['build:native'] = 'node native/build.mjs';
    scripts.build = 'npm run build:native && osfui build';
    scripts.package = 'npm run build:native && osfui package';
  }
  const packageJson = {
    name: slug(basename(root)),
    version: '0.1.0',
    private: true,
    type: 'module',
    scripts,
    devDependencies: { '@osfui/cli': cliSpec },
  };
  await put(root, 'package.json', `${JSON.stringify(packageJson, null, 2)}\n`);
  await put(root, '.gitignore', 'node_modules/\ndist/\nrelease/\n.osfui/\n');
  if (options.template === 'typescript') {
    await put(root, 'tsconfig.json', `${JSON.stringify({
      compilerOptions: {
        target: 'ES2022',
        module: 'ESNext',
        moduleResolution: 'Bundler',
        strict: true,
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
  }
  for (const file of backendFiles(options)) {
    await put(root, file.path, file.content);
  }
  if (options.integration === 'native') {
    const includeRoot = resolve(root, 'native/include');
    await mkdir(includeRoot, { recursive: true });
    for (const name of ['OSFUI_API.h', 'OSFUI_JSON.h']) {
      await cp(resolve(HERE, '..', `templates/native/${name}`), resolve(includeRoot, name));
    }
  }
  await put(root, `osfui.config.${extension}`, `import { defineConfig } from '@osfui/cli';

export default defineConfig({
  modId: '${options.modId}',
  views: [{
    id: '${options.view}',
    title: '${options.view.replaceAll('-', ' ')}',
    kind: '${options.surface}',
    width: ${options.surface === 'hud' ? 1920 : 1200},
    height: ${options.surface === 'hud' ? 1080 : 720},
    transparent: true,
    permissions: { nativeBridge: true },
  }],
});
`);
  await put(root, `osfui.mock.${extension}`, mockSource(options));
  await put(root, `${viewRoot}/index.html`, `<!doctype html>
<html lang="en"><head><meta charset="UTF-8"><meta name="viewport" content="width=device-width,initial-scale=1">
<title>${options.view}</title></head><body><div id="app"></div><script type="module" src="./main.${extension}"></script></body></html>
`);
  await put(root, `${viewRoot}/main.${extension}`, appSource(options));
  await put(root, `${viewRoot}/style.css`, `:root { font-family: system-ui, sans-serif; color: #eef7fb; background: transparent; }
* { box-sizing: border-box; }
body { margin: 0; min-height: 100vh; display: grid; place-items: center; }
.card { width: min(640px, 86vw); padding: 32px; background: rgba(8, 19, 27, .94); border: 1px solid #5aa8c7; }
h1 { margin: 4px 0 8px; }
p { color: #b8cbd4; }
.eyebrow { margin: 0; color: #7bdcff; letter-spacing: .18em; }
.state { display: grid; gap: 6px; margin: 22px 0; padding: 18px; background: rgba(90, 168, 199, .1); }
.state span { color: #7bdcff; text-transform: uppercase; letter-spacing: .12em; }
.state strong { font-size: 42px; }
.state small { color: #a9dced; }
.actions, form { display: flex; gap: 10px; margin-top: 12px; }
form { align-items: center; flex-wrap: wrap; }
input { padding: 10px; color: inherit; background: #101f27; border: 1px solid #426779; }
label { display: flex; align-items: center; gap: 5px; }
button { padding: 10px 16px; color: inherit; background: #173747; border: 1px solid #5aa8c7; cursor: pointer; }
button:disabled { cursor: not-allowed; opacity: .45; }
output { display: block; min-height: 24px; margin-top: 18px; color: #a9dced; overflow-wrap: anywhere; }
`);
  await put(root, 'README.md', `# ${packageJson.name}

Run \`npm run dev\` for instant browser HMR. Run \`npm run dev:game -- --deploy "path-to-MO2-mods"\`
to create this mod's folder under MO2 and sync into Starfield with temporary
author mode, F11 reload, and F12 DevTools.

Use \`npm run package\` to create a release-ready zip. Files under \`mod/\`
are copied into the mod archive beside the generated view.

${backendGuide(options)}
`);
  return root;
}

function install(root) {
  return new Promise((resolvePromise, reject) => {
    const executable = process.platform === 'win32' ? process.env.ComSpec || 'cmd.exe' : 'npm';
    const args = process.platform === 'win32' ? ['/d', '/s', '/c', 'npm install'] : ['install'];
    const child = spawn(executable, args, { cwd: root, stdio: 'inherit' });
    child.once('exit', (code) => code === 0 ? resolvePromise() : reject(new Error(`npm install exited with ${code}`)));
  });
}

async function main() {
  const options = parse(process.argv.slice(2));
  if (options.help) {
    console.log('npm create osfui@latest [directory] [-- --mod-id author.mod --view main --template typescript --surface menu --integration papyrus]');
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
