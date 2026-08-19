import assert from 'node:assert/strict';
import test from 'node:test';
import { CHOICES, promptMissing, validModId } from '../src/prompts.mjs';

test('offers only Papyrus and Native Plugin workflows', () => {
  assert.deepEqual(CHOICES.integration.map(({ value }) => value), ['papyrus', 'native']);
});

test('offers settings-only beside the two view starter types', () => {
  assert.deepEqual(CHOICES.surface.map(({ value }) => value), ['menu', 'hud', 'settings']);
});

test('accepts opaque mod ids while reserving osfui and unsafe path names', () => {
  for (const id of ['widgets', 'Acme Widgets', 'acme.widgets.v2', 'under_score!', 'Pilot\'s HUD']) {
    assert.equal(validModId(id), true, id);
  }
  for (const id of ['', 'osfui', 'OSFUI', '../evil', 'bad:name', 'NUL', '★'.repeat(22)]) {
    assert.equal(validModId(id), false, id);
  }
});

function recordingPrompt(textAnswers, selectAnswers, questions) {
  return {
    intro: (title) => questions.push({ kind: 'intro', title }),
    isCancel: () => false,
    text: async (question) => {
      questions.push({ kind: 'text', ...question });
      return textAnswers.shift();
    },
    select: async (question) => {
      questions.push({ kind: 'select', ...question });
      return selectAnswers.shift();
    },
  };
}

test('walks through missing choices as visible select lists', async () => {
  const questions = [];
  const textAnswers = ['custom-view', 'acme.widgets', 'panel'];
  const selectAnswers = ['hud', 'native'];
  const prompt = {
    intro: (title) => questions.push({ kind: 'intro', title }),
    isCancel: () => false,
    text: async (question) => {
      questions.push({ kind: 'text', ...question });
      return textAnswers.shift();
    },
    select: async (question) => {
      questions.push({ kind: 'select', ...question });
      return selectAnswers.shift();
    },
  };
  const options = {};

  const interactive = await promptMissing(
    options,
    prompt,
    { input: { isTTY: true }, output: { isTTY: true } },
  );

  assert.equal(interactive, true);
  assert.deepEqual(options, {
    directory: 'custom-view',
    modId: 'acme.widgets',
    view: 'panel',
    surface: 'hud',
    integration: 'native',
  });
  // Starter type is asked before View name so settings-only can skip it.
  assert.deepEqual(
    questions.filter(({ kind }) => kind !== 'intro').map(({ message }) => message),
    ['Directory name', 'Mod ID', 'Choose a starter type', 'View name', 'Choose a starting workflow'],
  );
  const textQuestions = questions.filter(({ kind }) => kind === 'text');
  assert.equal(textQuestions[0].defaultValue, 'my-osfui-view');
  assert.equal(textQuestions[0].placeholder, 'my-osfui-view');
  assert.equal(textQuestions[0].validate(''), undefined);
  assert.equal(
    textQuestions[0].validate('..'),
    'Use a single folder name, such as my-osfui-view.',
  );
  assert.equal(
    textQuestions[0].validate('nested/path'),
    'Use a single folder name, such as my-osfui-view.',
  );
  assert.equal(textQuestions[1].defaultValue, undefined);
  assert.equal(textQuestions[1].placeholder, 'custom-view');
  assert.equal(
    textQuestions[1].validate(''),
    'Use a safe mod name other than osfui (at most 64 UTF-8 bytes).',
  );
  assert.equal(textQuestions[2].defaultValue, 'main');
  assert.equal(textQuestions[2].placeholder, 'main');
  assert.equal(textQuestions[2].validate(''), undefined);
  assert.equal(
    textQuestions[2].validate('Bad View'),
    'Use lowercase letters, numbers, and hyphens.',
  );
  assert.deepEqual(
    questions.filter(({ kind }) => kind === 'select').map(({ message, options: choices }) => ({
      message,
      values: choices.map(({ value }) => value),
    })),
    [
      { message: 'Choose a starter type', values: CHOICES.surface.map(({ value }) => value) },
      { message: 'Choose a starting workflow', values: CHOICES.integration.map(({ value }) => value) },
    ],
  );
});

test('the settings-only starter skips the view and workflow prompts', async () => {
  const questions = [];
  const options = {};

  const interactive = await promptMissing(
    options,
    recordingPrompt(['custom-view', 'acme.widgets'], ['settings'], questions),
    { input: { isTTY: true }, output: { isTTY: true } },
  );

  assert.equal(interactive, true);
  // view stays at its default and is never used by the settings scaffold.
  assert.deepEqual(options, {
    directory: 'custom-view',
    modId: 'acme.widgets',
    view: 'main',
    surface: 'settings',
    integration: 'papyrus',
  });
  assert.deepEqual(
    questions.filter(({ kind }) => kind !== 'intro').map(({ message }) => message),
    ['Directory name', 'Mod ID', 'Choose a starter type'],
  );
});

test('keeps explicit flags and fills only missing values without a TTY', async () => {
  const options = {
    directory: 'widgets',
    surface: 'hud',
    integration: 'native',
  };

  const interactive = await promptMissing(
    options,
    {},
    { input: { isTTY: false }, output: { isTTY: false } },
  );

  assert.equal(interactive, false);
  assert.deepEqual(options, {
    directory: 'widgets',
    modId: 'widgets',
    view: 'main',
    surface: 'hud',
    integration: 'native',
  });
});
