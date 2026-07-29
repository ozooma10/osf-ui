import * as prompts from '@clack/prompts';
import { stdin, stdout } from 'node:process';
import { basename, resolve } from 'node:path';

export const ID = /^[a-z0-9-]+$/;
export const MOD_ID = /^(?:[a-z0-9-]+)\.(?:[a-z0-9-]+)$/;

export const CHOICES = {
  template: [
    { value: 'typescript', label: 'TypeScript', hint: 'strict types and editor checking' },
    { value: 'javascript', label: 'JavaScript', hint: 'plain browser modules' },
  ],
  surface: [
    { value: 'menu', label: 'Menu', hint: 'a focused screen with user input' },
    { value: 'hud', label: 'HUD', hint: 'an overlay shown during gameplay' },
  ],
  integration: [
    { value: 'papyrus', label: 'Papyrus', hint: 'send requests to Papyrus scripts' },
    { value: 'native', label: 'Native plugin', hint: 'call your SFSE plugin bridge' },
  ],
};

export class PromptCancelledError extends Error {}

function answer(prompt, value) {
  if (!prompt.isCancel(value)) return value;
  prompt.cancel('Operation cancelled.');
  throw new PromptCancelledError();
}

export const slug = (value) =>
  value.toLowerCase().replace(/[^a-z0-9]+/g, '-').replace(/^-|-$/g, '') || 'my-view';

function fillDefaults(options) {
  options.directory ||= 'my-osfui-view';
  const projectName = slug(basename(resolve(options.directory)));
  options.modId ||= `yourname.${projectName}`;
  options.view ||= 'main';
  options.template ||= 'typescript';
  options.surface ||= 'menu';
  options.integration ||= 'papyrus';
}

export async function promptMissing(
  options,
  prompt = prompts,
  terminal = { input: stdin, output: stdout },
) {
  const interactive = !options.yes && terminal.input.isTTY && terminal.output.isTTY;
  if (!interactive) {
    fillDefaults(options);
    return false;
  }

  prompt.intro('Create an OSF UI view');

  options.directory ||= answer(prompt, await prompt.text({
    message: 'Directory name',
    defaultValue: 'my-osfui-view',
    validate: (value) => value && value !== '.' && value !== '..' && !/[<>:"/\\|?*]/.test(value)
      ? undefined
      : 'Use a single folder name, such as my-osfui-view.',
  }));

  const projectName = slug(basename(resolve(options.directory)));
  options.modId ||= answer(prompt, await prompt.text({
    message: 'Mod ID',
    placeholder: `yourname.${projectName}`,
    validate: (value) => MOD_ID.test(value)
      ? undefined
      : 'Use lowercase author.mod-name format.',
  }));

  options.view ||= answer(prompt, await prompt.text({
    message: 'View ID',
    defaultValue: 'main',
    validate: (value) => ID.test(value)
      ? undefined
      : 'Use lowercase letters, numbers, and hyphens.',
  }));

  options.template ||= answer(prompt, await prompt.select({
    message: 'Choose a language',
    options: CHOICES.template,
    initialValue: 'typescript',
  }));

  options.surface ||= answer(prompt, await prompt.select({
    message: 'Choose a surface',
    options: CHOICES.surface,
    initialValue: 'menu',
  }));

  options.integration ||= answer(prompt, await prompt.select({
    message: 'Choose a starting workflow',
    options: CHOICES.integration,
    initialValue: 'papyrus',
  }));

  return true;
}

export function finishPrompt(message, prompt = prompts) {
  prompt.outro(message);
}
