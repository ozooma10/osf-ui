
import './style.css';
import { render } from 'preact';
import { windowBridge } from '@lib/bridge';
import { App } from './App';

if (import.meta.env.DEV) {
  if (!windowBridge.available()) {
    console.info('[osfui/keybinds] no bridge — standalone preview with sample data');
  }
}

render(<App bridge={windowBridge} />, document.getElementById('app')!);
