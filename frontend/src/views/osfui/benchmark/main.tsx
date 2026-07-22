import './style.css';
import { render } from 'preact';
import { windowBridge } from '@lib/bridge';
import { App } from './App';

render(<App bridge={windowBridge} />, document.getElementById('app')!);
