
import './style.css';
import { render } from 'preact';
import { windowBridge } from '@lib/bridge';
import { App } from './App';

if (import.meta.env.DEV) {
  if (!windowBridge.available()) {
    console.info('[osfui/settings] no bridge — the harness supplies data via the mock');
  }
}

render(<App bridge={windowBridge} />, document.getElementById('app')!);
