// @vitest-environment jsdom
import { it, expect } from 'vitest';
import { readFileSync } from 'node:fs';
import { resolve } from 'node:path';

it('renders only owner-forwarded state and closes through the existing web bridge', () => {
  const root = resolve(process.cwd(), '../examples/settings-view/data/SFSE/Plugins/OSF/UI/views/osfui-example/panel');
  document.documentElement.innerHTML = readFileSync(resolve(root, 'index.html'), 'utf8');
  const sent: Array<{ name: string }> = [];
  (window as unknown as { osfui: unknown }).osfui = { postMessage: (json: string) => sent.push(JSON.parse(json)) };
  new Function(readFileSync(resolve(process.cwd(), 'src/shared-kit/osfui.js'), 'utf8'))();
  new Function(readFileSync(resolve(root, 'main.js'), 'utf8'))();
  const helper = window.osfui as unknown as { onMessage: (json: string) => void };
  const deliver = (frame: unknown) => helper.onMessage(JSON.stringify(frame));
  deliver({ kind: 'ready', payload: { mod: 'osfui-example' } });
  deliver({ kind: 'state', mod: 'osfui-example', key: 'showDetails', value: true });
  expect(document.getElementById('details')!.hidden).toBe(false);
  expect(document.getElementById('status')!.textContent).toBe('Details are on.');
  deliver({ kind: 'state', mod: 'other', key: 'showDetails', value: false });
  expect(document.getElementById('details')!.hidden).toBe(false);
  deliver({ kind: 'state', mod: 'osfui-example', key: 'showDetails', value: false });
  expect(document.getElementById('details')!.hidden).toBe(true);
  expect(sent.map(frame => frame.name)).toEqual(['osfui.hello']);
  document.getElementById('close')!.click();
  document.dispatchEvent(new KeyboardEvent('keydown', { key: 'Escape' }));
  expect(sent.map(frame => frame.name)).toEqual(['osfui.hello', 'close', 'close']);
});
