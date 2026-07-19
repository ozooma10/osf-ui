// install-mock.ts — side-effect installer for the DEV import chain. DEV ONLY.
//
// This file exists for ONE reason: import order. The shared kit
// (src/shared-kit/osfui.js) decides whether a bridge exists by looking for
// `window.osfui.postMessage` at load time, and then takes ownership of
// `onMessage`. So the mock must be installed BEFORE the kit's module is
// evaluated.
//
// ES module evaluation follows import-declaration order, so main.tsx writes:
//
//   import './install-mock';               // 1. window.osfui.postMessage
//   import '../src/shared-kit/osfui.js';   // 2. the kit decorates it
//   import '../src/legacy/padnav.js';      // 3. focus navigation
//
// Doing it with a bare `installMock()` call inside main.tsx would NOT work:
// every static import in a module is evaluated before any of its own top-level
// statements run, so the kit would load first and conclude there is no bridge.

import { installMock, type MockApi } from './mockbridge';

/** The installed mock. Also reachable as `window.osfui._mock` from a console. */
export const mock: MockApi = installMock();
