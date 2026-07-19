// main.tsx — the keybinds view entry point.
//
// Production shape is deliberately tiny: import the stylesheet (Vite extracts it
// to the sibling style.css), mount the app against the real bridge, done.
//
// The bundle is ONE CLASSIC IIFE — no ESM, no dynamic import(), no code
// splitting. Ultralight loads views over `file:///` (an opaque origin) where
// module scripts are CORS-blocked and the view would render blank.
//
// index.html loads three classic scripts in a fixed order before this one:
// shared/osfui.js (the frozen helper that owns `window.osfui`), ../padnav.js
// (gamepad/arrow focus navigation, which reads the DOM this app produces), then
// main.js. `windowBridge` is a typed façade over the helper — see @lib/bridge.

import './style.css';
import { render } from 'preact';
import { windowBridge } from '@lib/bridge';
import { App } from './App';

if (import.meta.env.DEV) {
  // Dev-only diagnostics, eliminated from the shipped IIFE by esbuild.
  //
  // There is no mock-bridge import HERE on purpose. The dev harness owns that
  // chain (harness/install-mock.ts documents why it has to be an import and not
  // a call: the shared kit decides whether a bridge exists at module-evaluation
  // time, so the mock must be installed by an EARLIER import declaration). The
  // harness page mounts <App> itself; this file is the production entry and is
  // never the one running under `vite dev`.
  //
  // Both paths therefore mount against `windowBridge` — the mock decorates the
  // same `window.osfui` the native runtime injects. The only other dev-only
  // behaviour, the standalone sample data for a bridge-less plain browser,
  // lives in App.tsx behind this same flag.
  if (!windowBridge.available()) {
    console.info('[osfui/keybinds] no bridge — standalone preview with sample data');
  }
}

render(<App bridge={windowBridge} />, document.getElementById('app')!);
