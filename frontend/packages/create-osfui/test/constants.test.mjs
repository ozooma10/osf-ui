import assert from 'node:assert/strict';
import test from 'node:test';

import {
  MAX_MOD_ID_LENGTH,
  OSFUI_RELEASE_VERSION,
  isThirdPartyModId,
} from '../src/constants.mjs';
import {
  MAX_MOD_ID_LENGTH as CLI_MAX_MOD_ID_LENGTH,
  OSFUI_RELEASE_VERSION as CLI_OSFUI_RELEASE_VERSION,
  isAcceptedModId,
} from '../../cli/src/constants.mjs';

test('keeps the scaffold release and mod-id cap aligned with the sibling CLI', () => {
  assert.equal(OSFUI_RELEASE_VERSION, CLI_OSFUI_RELEASE_VERSION);
  assert.equal(MAX_MOD_ID_LENGTH, CLI_MAX_MOD_ID_LENGTH);
});

test('uses the CLI mod-id grammar while reserving the OSF UI owner id', () => {
  for (const value of [
    '', 'widgets', 'Acme Widgets', 'Pilot\'s HUD', 'under_score!',
    'osfui', 'OSFUI', '../evil', 'bad:name', 'NUL', '★'.repeat(22),
  ]) {
    assert.equal(
      isThirdPartyModId(value),
      isAcceptedModId(value) && value.toLowerCase() !== 'osfui',
      value,
    );
  }
});
