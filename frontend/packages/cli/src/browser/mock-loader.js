import { installMock } from './mock-runtime.js';

const harness = window.__osfuiHarness;

let mod = {};
let loadError = null;
try {
  mod = harness.meta.mockUrl
    ? await import(/* @vite-ignore */ harness.meta.mockUrl)
    : {};
} catch (error) {
  loadError = error;
}

try {
  await installMock(harness, mod, loadError);
} catch (error) {
  harness.status(false, error instanceof Error ? error.message : String(error));
}
