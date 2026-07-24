// Handoff view entry point: import the stylesheet (Vite extracts it to the
// sibling style.css), mount the app against the real bridge.
//
// The built-in artifact contract is one classic IIFE — no dynamic import() or
// code splitting.
//
// index.html loads shared/osfui.js (the frozen helper that owns `window.osfui`)
// as a classic script before main.js. Unlike settings/keybinds this view does
// not load padnav.js: it has at most two focusable controls, and Retry is
// focused programmatically when it appears.

import './style.css';
import { render } from 'preact';
import { windowBridge } from '@lib/bridge';
import { App } from './App';

render(<App bridge={windowBridge} />, document.getElementById('app')!);
