import assert from 'node:assert/strict';
import test from 'node:test';

import {
  appendLegacyApi,
  mergeHarnessViewUrl,
} from '../src/browser/legacy-navigation.js';

test('legacy preview selection preserves an authored query and fragment', () => {
  assert.equal(
    appendLegacyApi('/acme.widgets/panel/index.html?mode=compact#inventory'),
    '/acme.widgets/panel/index.html?mode=compact&osfui-api=1#inventory',
  );
});

test('harness forwarding preserves the compatibility flag and authored fragment', () => {
  assert.equal(
    mergeHarnessViewUrl(
      '/acme.widgets/panel/index.html?mode=compact&osfui-api=1#inventory',
      'http://localhost:5173/__osfui/?view=acme.widgets%2Fpanel&locale=fr',
    ),
    '/acme.widgets/panel/index.html?mode=compact&osfui-api=1&locale=fr#inventory',
  );
});

test('an explicit shell fragment overrides the authored fragment', () => {
  assert.equal(
    mergeHarnessViewUrl(
      '/acme.widgets/panel/index.html?osfui-api=1#inventory',
      'http://localhost:5173/__osfui/?scenario=empty#details',
    ),
    '/acme.widgets/panel/index.html?osfui-api=1&scenario=empty#details',
  );
});
