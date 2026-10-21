import assert from 'node:assert/strict';
import { readFileSync } from 'node:fs';
import vm from 'node:vm';
import test from 'node:test';

const source = readFileSync(new URL('../data/SFSE/Plugins/OSF/UI/views/osfui.browser/tv/chrome.js', import.meta.url), 'utf8');

function chrome() {
  let writes = 0;
  const elements = new Map(), events = new Map(), stored = new Map();
  const document = { activeElement: null };
  function node() {
    const attributes = new Map();
    return new Proxy({
      children: [], dataset: {}, value: '', textContent: '', hidden: false,
      classList: { toggle() { writes++; }, add() { writes++; }, remove() { writes++; } },
      style: { setProperty() { writes++; } },
      setAttribute(key, value) { writes++; attributes.set(key, value); },
      getAttribute(key) { return attributes.get(key); },
      append(...children) { writes++; this.children.push(...children); },
      replaceChildren(...children) { writes++; this.children = children; },
      insertAdjacentHTML() { writes++; }, addEventListener() {},
      querySelectorAll() { return []; }, querySelector() { return null; },
      focus() { document.activeElement = this; }, select() {}, closest() { return null; },
    }, { set(target, key, value) { writes++; target[key] = value; return true; } });
  }
  Object.assign(document, {
    body: node(), createElement: node, addEventListener() {},
    getElementById(id) { if (!elements.has(id)) elements.set(id, node()); return elements.get(id); },
  });
  vm.runInNewContext(source, {
    document, URL, setTimeout, clearTimeout,
    localStorage: {
      getItem(key) { return stored.get(key); },
      setItem(key, value) { writes++; stored.set(key, value); },
      removeItem(key) { writes++; stored.delete(key); },
    },
    osfui: { on(name, callback) { events.set(name, callback); }, send() {},
      request() { return new Promise(() => {}); } },
  });
  return { apply: events.get('browser.state'), element: id => document.getElementById(id),
    writes: () => writes };
}

const playing = { enabled: true, landing: false, fullscreen: true, url: 'https://example.test/video',
  title: 'Example', secure: true, loading: false, canGoBack: false, muted: false,
  audible: true, audibleKnown: true, screens: 2, audio: { packets: 10 } };

test('audio-only updates leave the DOM, storage and address edits alone', () => {
  const ui = chrome();
  ui.apply(playing);
  ui.element('readout').onclick();
  ui.element('address').value = 'unfinished user input';
  const before = ui.writes();
  for (let packets = 11; packets < 111; packets++) ui.apply({ ...playing, audio: { packets, spatial: { listener: [packets, 0, 0] } } });
  assert.equal(ui.writes(), before);
  assert.equal(ui.element('address').value, 'unfinished user input');
  assert.equal(ui.element('mute-label').textContent, 'Playing');
});

test('mute, audibility, loading, screen count and navigation still update controls', () => {
  const ui = chrome();
  ui.apply(playing);
  ui.apply({ ...playing, muted: true, screens: 3, loading: true, title: 'Next', canGoBack: true });
  assert.equal(ui.element('mute-label').textContent, 'Muted');
  assert.equal(ui.element('screens-label').textContent, 'On 3 bound screens');
  assert.equal(ui.element('reload').title, 'Stop loading');
  assert.equal(ui.element('page-title').textContent, 'Next');
  assert.equal(ui.element('back').disabled, false);
  ui.apply({ ...playing, audible: false });
  assert.equal(ui.element('mute-label').textContent, 'Sound on');
  assert.equal(ui.element('reload').title, 'Reload');
});

test('landing and screen-count changes remain visible after diagnostic updates', () => {
  const ui = chrome();
  const home = { ...playing, landing: true, fullscreen: false, url: 'about:blank', title: '' };
  ui.apply(home);
  ui.apply({ ...home, audio: { packets: 20 } });
  ui.apply({ ...home, screens: 1 });
  assert.equal(ui.element('status-screens').textContent, '1 bound screen on this channel');
  ui.apply({ ...home, enabled: false });
  assert.equal(ui.element('section-title').textContent, 'Web browsing is off');
});
