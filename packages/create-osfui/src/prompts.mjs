import * as prompts from '@clack/prompts';
import { stdin, stdout } from 'node:process';
import { basename, resolve } from 'node:path';
import { MAX_MOD_ID_LENGTH, isThirdPartyModId } from '@osfui/cli/constants';

export const ID = /^[a-z0-9-]+$/;
export const validModId = (value) => isThirdPartyModId(value);

export const CHOICES = {
  surface: [
    { value: 'menu', label: 'Menu', hint: 'a focused screen with user input' },
    { value: 'hud', label: 'HUD', hint: 'a persistent view shown during gameplay' },
    { value: 'settings', label: 'Settings only', hint: 'a settings page and a hotkey, no view code' },
  ],
  integration: [
    { value: 'papyrus', label: 'Papyrus', hint: 'call GLOBAL functions on loose scripts' },
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
  options.modId ||= projectName;
  options.view ||= 'main';
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

  prompt.intro('Create an OSF UI starter');

  options.directory ||= answer(prompt, await prompt.text({
    message: 'Directory name',
    placeholder: 'my-osfui-view',
    defaultValue: 'my-osfui-view',
    validate: (value) => !value || (value !== '.' && value !== '..' && !/[<>:"/\\|?*]/.test(value))
      ? undefined
      : 'Use a single folder name, such as my-osfui-view.',
  }));

  const projectName = slug(basename(resolve(options.directory)));
  options.modId ||= answer(prompt, await prompt.text({
    message: 'Mod ID',
    placeholder: projectName,
    validate: (value) => validModId(value)
      ? undefined
      : `Use a safe mod name other than osfui (at most ${MAX_MOD_ID_LENGTH} UTF-8 bytes).`,
  }));

  // Starter type comes before View name and workflow because "settings only"
  // answers both of them: it ships no view, and Papyrus is its only mod backend.
  // `surface` remains the stable CLI/config option name for compatibility.
  options.surface ||= answer(prompt, await prompt.select({
    message: 'Choose a starter type',
    options: CHOICES.surface,
    initialValue: 'menu',
  }));

  if (options.surface === 'settings') {
    fillDefaults(options);
    options.integration = 'papyrus';
    return true;
  }

  options.view ||= answer(prompt, await prompt.text({
    message: 'View name',
    placeholder: 'main',
    defaultValue: 'main',
    validate: (value) => !value || ID.test(value)
      ? undefined
      : 'Use lowercase letters, numbers, and hyphens.',
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
