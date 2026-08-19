#!/usr/bin/env node
import { cp, mkdir, readdir } from 'node:fs/promises';
import { basename, dirname, resolve } from 'node:path';
import { spawn } from 'node:child_process';
import { fileURLToPath } from 'node:url';
import {
  finishPrompt,
  ID,
  promptMissing,
  PromptCancelledError,
  slug,
  validModId,
} from './prompts.mjs';
import { MAX_MOD_ID_LENGTH, OSFUI_RELEASE_VERSION } from '@osfui/cli/constants';
import { resolveCliSpec } from './cli-spec.mjs';
import { renderProjectTemplate } from './project-template.mjs';

const HERE = dirname(fileURLToPath(import.meta.url));

// Closed sets: a typo'd flag ("--surfce hud") must fail here, not scaffold
// the default starter type and exit 0. `--surface` remains the stable flag name.
const VALUE_FLAGS = {
  '--mod-id': 'modId',
  '--view': 'view',
  '--surface': 'surface',
  '--integration': 'integration',
  '--cli-spec': 'cliSpec',
};
const BOOLEAN_FLAGS = { '--yes': 'yes', '--no-install': 'noInstall', '--help': 'help' };

function parse(argv) {
  const result = { _: [] };
  for (let index = 0; index < argv.length; index++) {
    const value = argv[index];
    if (!value.startsWith('--')) {
      result._.push(value);
      continue;
    }
    if (BOOLEAN_FLAGS[value]) {
      result[BOOLEAN_FLAGS[value]] = true;
      continue;
    }
    const key = VALUE_FLAGS[value];
    if (!key) {
      throw new Error(`Unknown option "${value}". Known options: ` +
        `${[...Object.keys(VALUE_FLAGS), ...Object.keys(BOOLEAN_FLAGS)].join(', ')}.`);
    }
    const next = argv[++index];
    if (next === undefined || next.startsWith('--')) {
      throw new Error(`Missing value for ${value}.`);
    }
    result[key] = next;
  }
  return result;
}

function validate(options) {
  if (!validModId(options.modId)) {
    throw new Error(`--mod-id must be a safe name other than osfui and at most ${MAX_MOD_ID_LENGTH} UTF-8 bytes.`);
  }
  if (!ID.test(options.view)) throw new Error('--view must use lowercase letters, digits, and hyphens.');
  if (!['menu', 'hud', 'settings'].includes(options.surface)) {
    throw new Error('--surface must be menu, hud, or settings.');
  }
  if (!['papyrus', 'native'].includes(options.integration)) {
    throw new Error('--integration must be papyrus or native.');
  }
  // A settings row's only code path back into the game is Papyrus: onPress
  // targets a GLOBAL function, and an action row needs a whole SFSE plugin to
  // answer its request — that is the menu/native preset, not this one.
  if (options.surface === 'settings' && options.integration !== 'papyrus') {
    throw new Error('--surface settings is Papyrus-only; ' +
      'use --surface menu --integration native for an SFSE-plugin project.');
  }
}

async function copyPapyrusApi(root) {
  const papyrusRoot = resolve(root, 'tools/papyrus');
  await mkdir(papyrusRoot, { recursive: true });
  await cp(
    resolve(HERE, '..', 'templates/papyrus/OSFUI.psc'),
    resolve(papyrusRoot, 'OSFUI.psc'),
  );
}

export async function scaffold(options) {
  const root = resolve(options.directory);
  await mkdir(root, { recursive: true });
  if ((await readdir(root)).length) throw new Error(`Directory is not empty: ${root}`);

  const cliSpec = options.surface === 'settings'
    ? ''
    : await resolveCliSpec(root, options.cliSpec);
  await renderProjectTemplate(root, {
    ...options,
    cliSpec,
    projectName: slug(basename(root)),
    releaseVersion: OSFUI_RELEASE_VERSION,
  });

  if (options.integration === 'native') {
    const includeRoot = resolve(root, 'native/include');
    await mkdir(includeRoot, { recursive: true });
    for (const name of ['OSFUI_API.h', 'OSFUI_JSON.h']) {
      await cp(resolve(HERE, '..', `templates/native/${name}`), resolve(includeRoot, name));
    }
  } else {
    await copyPapyrusApi(root);
  }
  return root;
}

function install(root) {
  return new Promise((resolvePromise, reject) => {
    const executable = process.platform === 'win32' ? process.env.ComSpec || 'cmd.exe' : 'npm';
    const args = process.platform === 'win32' ? ['/d', '/s', '/c', 'npm install'] : ['install'];
    const child = spawn(executable, args, { cwd: root, stdio: 'inherit' });
    child.once('exit', (code) => code === 0
      ? resolvePromise()
      : reject(new Error(`npm install exited with ${code}`)));
  });
}

async function main() {
  const options = parse(process.argv.slice(2));
  if (options.help) {
    console.log('npm create osfui@latest [directory] ' +
      '[-- --mod-id my-mod --view main --surface menu|hud|settings --integration papyrus|native]');
    return;
  }
  options.directory = options._[0];
  const interactive = await promptMissing(options);
  validate(options);
  const root = await scaffold(options);
  // The settings-only starter has no package.json to install into.
  if (!options.noInstall && options.surface !== 'settings') await install(root);
  const firstCommands = options.surface === 'settings'
    ? './build-deploy.ps1 -Mo2Mods "path-to-MO2-mods"'
    : options.integration === 'papyrus'
      ? 'npm run doctor\n  npm run dev'
      : 'npm run dev';
  const next = root === process.cwd()
    ? firstCommands
    : `cd ${options.directory}\n  ${firstCommands}`;
  const result = `Created ${root}\n\nNext:\n  ${next}`;
  if (interactive) finishPrompt(result);
  else console.log(`\n${result}`);
}

main().catch((error) => {
  if (error instanceof PromptCancelledError) return;
  console.error(`create-osfui: ${error.message}`);
  process.exitCode = 1;
});
