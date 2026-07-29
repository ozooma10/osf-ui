import assert from 'node:assert/strict';
import test from 'node:test';
import { CHOICES, promptMissing } from '../src/prompts.mjs';

test('offers only Papyrus and Native Plugin workflows', () => {
  assert.deepEqual(CHOICES.integration.map(({ value }) => value), ['papyrus', 'native']);
});

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
  assert.deepEqual(
    questions.filter(({ kind }) => kind === 'text').map(({ message }) => message),
    ['Directory name', 'Mod ID', 'View ID'],
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
  assert.equal(textQuestions[1].placeholder, 'yourname.custom-view');
  assert.equal(textQuestions[1].validate(''), 'Use lowercase author.mod-name format.');
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
      { message: 'Choose a surface', values: CHOICES.surface.map(({ value }) => value) },
      { message: 'Choose a starting workflow', values: CHOICES.integration.map(({ value }) => value) },
    ],
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
    modId: 'yourname.widgets',
    view: 'main',
    surface: 'hud',
    integration: 'native',
  });
});
