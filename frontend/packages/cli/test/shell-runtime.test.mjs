import assert from 'node:assert/strict';
import test from 'node:test';

function element(tagName = 'div') {
  const classes = new Set();
  return {
    tagName,
    children: [],
    listeners: {},
    textContent: '',
    title: '',
    style: { setProperty() {} },
    classList: {
      toggle(name, force) {
        if (force === undefined ? !classes.has(name) : force) classes.add(name);
        else classes.delete(name);
      },
      contains(name) { return classes.has(name); },
    },
    append(...children) { this.children.push(...children); },
    replaceChildren(...children) { this.children = children; },
    addEventListener(name, listener) { this.listeners[name] = listener; },
    setAttribute(name, value) { this[name] = value; },
  };
}

test('toggle controls update optimistically and runtime failure survives iframe load',
  { concurrency: false }, async (t) => {
    const names = ['window', 'document', 'location', 'fetch', 'ResizeObserver'];
    const saved = Object.fromEntries(names.map((name) => [name, globalThis[name]]));
    t.after(() => {
      for (const [name, value] of Object.entries(saved)) {
        if (value === undefined) delete globalThis[name];
        else globalThis[name] = value;
      }
    });

    const sent = [];
    const frame = element('iframe');
    frame.contentWindow = {
      postMessage(message) { sent.push(message); },
      location: { reload() {} },
    };
    const stage = element();
    stage.parentElement = { getBoundingClientRect: () => ({ width: 1600, height: 900 }) };
    const tools = element();
    const status = element();
    const viewId = element();
    const reload = element('button');
    const selectors = new Map([
      ['#view', frame], ['#stage', stage], ['#tools', tools], ['#status', status],
      ['#view-id', viewId], ['#reload', reload],
    ]);
    const windowListeners = {};
    globalThis.location = { origin: 'http://osfui.local' };
    globalThis.fetch = async () => ({
      json: async () => ({
        initial: 'acme.widgets/panel',
        views: [{
          qualifiedId: 'acme.widgets/panel', width: 1600, height: 900,
          viewUrl: '/acme.widgets/panel/index.html',
        }],
      }),
    });
    globalThis.document = {
      querySelector(selector) { return selectors.get(selector); },
      createElement(tagName) { return element(tagName); },
    };
    globalThis.window = {
      addEventListener(name, listener) { windowListeners[name] = listener; },
    };
    globalThis.ResizeObserver = class {
      constructor(callback) { this.callback = callback; }
      observe() {}
    };

    await import(`${new URL('../src/browser/shell.js', import.meta.url).href}?shell-test`);
    windowListeners.message({
      origin: location.origin,
      data: {
        source: 'osfui-harness',
        kind: 'tools',
        tools: [{ id: 'enabled', kind: 'toggle', label: 'Enabled', value: true }],
      },
    });

    tools.children[0].children[0].listeners.click();
    assert.equal(sent.at(-1).value, false);
    assert.match(tools.children[0].children[0].textContent, /Off$/);
    tools.children[0].children[0].listeners.click();
    assert.equal(sent.at(-1).value, true);
    assert.match(tools.children[0].children[0].textContent, /On$/);

    windowListeners.message({
      origin: location.origin,
      data: { source: 'osfui-harness', kind: 'mock-status', ok: false, message: 'broken mock' },
    });
    frame.listeners.load();
    assert.equal(status.textContent, 'Mock failed: broken mock');
  });
