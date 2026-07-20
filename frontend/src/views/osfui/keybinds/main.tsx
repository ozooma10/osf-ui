// Keybinds view entry point: import the stylesheet (Vite extracts it to the
// sibling style.css), mount the app against the real bridge.
//
// The built-in artifact contract is one classic IIFE — no dynamic import() or
// code splitting.
//
// index.html loads three classic scripts in this order before main.js:
// shared/osfui.js (the frozen helper that owns `window.osfui`), then
// ../padnav.js (gamepad/arrow focus navigation, which reads the DOM this app
// produces). `windowBridge` is a typed façade over the helper (@lib/bridge).

import './style.css';
import { render } from 'preact';
import { windowBridge } from '@lib/bridge';
import { App } from './App';

if (import.meta.env.DEV) {
  // Dev-only diagnostics, eliminated from the shipped IIFE by esbuild.
  //
  // The mock bridge is not imported here: the shared kit decides whether a
  // bridge exists at module-evaluation time, so the mock must be installed by
  // an earlier import declaration (harness/install-mock.ts). The harness page
  // mounts <App> itself; this file is the production entry.
  //
  // Both paths mount against `windowBridge` — the mock decorates the same
  // `window.osfui` the native runtime injects. The other dev-only behaviour,
  // standalone sample data for a bridge-less browser, lives in App.tsx behind
  // this same flag.
  if (!windowBridge.available()) {
    console.info('[osfui/keybinds] no bridge — standalone preview with sample data');
  }
}

render(<App bridge={windowBridge} />, document.getElementById('app')!);
