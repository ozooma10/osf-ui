// Side-effect installer for the dev import chain. Dev only.
//
// Exists purely for import order: the shared kit (src/shared-kit/osfui.js)
// decides whether a bridge exists by looking for `window.osfui.postMessage` at
// load time and then takes ownership of `onMessage`, so the mock must be
// installed before the kit's module is evaluated. ES module evaluation follows
// import-declaration order, so main.tsx writes:
//
//   import './install-mock';               // 1. window.osfui.postMessage
//   import '../src/shared-kit/osfui.js';   // 2. the kit decorates it
//   import '../src/legacy/padnav.js';      // 3. focus navigation
//
// A bare `installMock()` call inside main.tsx would not work: every static
// import is evaluated before any of the module's own top-level statements, so
// the kit would load first and conclude there is no bridge.

import { installMock, type MockApi } from './mockbridge';

/** The installed mock. Also reachable as `window.osfui._mock` from a console. */
export const mock: MockApi = installMock();
