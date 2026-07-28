// Ported from the frontend harness's stage tests: the fill mode has to model
// what the runtime does to a view (pin the scale to the reference row height,
// let the width follow the output aspect) or it teaches the wrong layout.

import assert from 'node:assert/strict';
import test from 'node:test';

import { STAGE_MODES, computeFit, nextStageMode } from '../src/browser/stage-fit.js';

test('fill pins the scale to the reference row height and covers the pane', () => {
  const fit = computeFit(2400, 900, 900);
  assert.equal(fit.scale, 1);
  assert.equal(fit.height, 900);
  assert.equal(fit.width, 2400);
});

test('fill does not cap at 1:1 — 1.2x in a 1080p pane is the in-game text size', () => {
  assert.equal(computeFit(1920, 1080, 900).scale, 1.2);
});

test('fill honors a non-default declared height', () => {
  const fit = computeFit(800, 600, 600);
  assert.equal(fit.scale, 1);
  assert.equal(fit.width, 800);
  assert.equal(fit.height, 600);
});

test('fill covers the pane exactly at any aspect', () => {
  for (const [w, h] of [[1280, 800], [1920, 1080], [3440, 1440], [800, 1200]]) {
    const fit = computeFit(w, h, 900);
    assert.ok(Math.abs(fit.scale - h / 900) < 1e-9);
    assert.ok(Math.abs(fit.width * fit.scale - w) < 1e-6);
    assert.ok(Math.abs(fit.height * fit.scale - h) < 1e-6);
  }
});

test('degenerate panes and dimensions never produce a zero or negative scale', () => {
  for (const [w, h] of [[0, 0], [-5, 10], [200, 1]]) {
    const fit = computeFit(w, h, 900);
    assert.ok(fit.scale > 0 && Number.isFinite(fit.scale), `${w}x${h}`);
  }
  const fit = computeFit(1920, 1080, 0);
  assert.ok(fit.scale > 0 && Number.isFinite(fit.scale));
});

test('mode cycle: fill -> off -> fill, unknown resets to fill', () => {
  assert.equal(nextStageMode('fill'), 'off');
  assert.equal(nextStageMode('off'), 'fill');
  assert.equal(nextStageMode('bogus'), 'fill');
  assert.deepEqual(STAGE_MODES, ['fill', 'off']);
});
