// Injected into every served view page as a MODULE script, directly after the
// classic bootstrap. Module scripts execute in order and honor top-level
// await, so a module view entry (the scaffolded shape) runs only after the
// mock has settled; a classic entry runs earlier but its sends and requests wait
// in the bootstrap's outbound queue. Either way the mock misses no endpoint call.
//
// The mock itself is imported through Vite (/__osfui/mock-entry.js resolves to
// the project's mock file), so TypeScript, aliases, and JSON all just work.

import { installMock } from './mock-runtime.js';

const harness = window.__osfuiHarness;
let mod = {};
let error = null;
try {
  if (harness.meta.mockUrl) mod = await import(/* @vite-ignore */ harness.meta.mockUrl);
} catch (cause) {
  error = cause;
}
try {
  await installMock(harness, mod, error);
} catch (cause) {
  // The mock runtime itself failed — report it and boot the view bare so the page
  // never white-screens.
  harness.status(false, 'Mock runtime failed: ' + (cause && cause.message ? cause.message : cause));
  harness.report('in', 'Mock runtime failed: ' + (cause && cause.stack ? cause.stack : cause), 'warn');
  harness.setHandler(() => {});
  harness.previewInitialized();
}
