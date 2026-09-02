import assert from 'node:assert/strict';
import test from 'node:test';

import { CHOICES, promptMissing } from '../src/prompts.mjs';

test('offers only web-view starters and supported integrations', () => {
  assert.deepEqual(CHOICES.surface.map(({ value }) => value), ['menu']);
  assert.deepEqual(CHOICES.integration.map(({ value }) => value), ['papyrus', 'native']);
});

test('fills non-interactive defaults without prompting', async () => {
  const options = { yes: true };
  const interactive = await promptMissing(options);
  assert.equal(interactive, false);
  assert.equal(options.surface, 'menu');
  assert.equal(options.integration, 'papyrus');
  assert.equal(options.view, 'main');
});
