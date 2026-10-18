// Exercise the installed, unchanged companion log through the shipped bridge.
// This checks messages and rendering logic, not browser layout or game data.
const { test } = require('node:test');
const assert = require('node:assert/strict');
const fs = require('node:fs');
const path = require('node:path');
const vm = require('node:vm');

test('original AISS sends populated memory requests and renders memory replies',
  { skip: !process.env.OSFUI_AISS_ROOT }, async () => {
    class Element {
      constructor() {
        this.children = []; this.listeners = {}; this.style = {}; this.value = '';
        this.classList = { add() {}, remove() {} };
      }
      set innerHTML(value) { this.html = value; this.children = []; }
      get innerHTML() { return this.html || ''; }
      appendChild(child) { this.children.push(child); }
      addEventListener(name, callback) { this.listeners[name] = callback; }
    }
    const elements = new Map(), sent = [];
    const document = {
      getElementById(id) { if (!elements.has(id)) elements.set(id, new Element()); return elements.get(id); },
      createElement() { return new Element(); }, addEventListener() {}, visibilityState: 'visible'
    };
    const window = { addEventListener() {}, osfui: { postMessage: text => sent.push(JSON.parse(text)) } };
    const context = vm.createContext({ window, document, console, setTimeout, clearTimeout });
    vm.runInContext(fs.readFileSync('src/Compat/V1/web/osfui.js', 'utf8'), context);
    context.osfui = window.osfui;
    vm.runInContext(fs.readFileSync(path.join(process.env.OSFUI_AISS_ROOT,
      'SFSE/Plugins/OSFUI/views/x2357aiss.companionlog/companionlog/main.js'), 'utf8'), context);
    const receive = message => window.osfui.onMessage(JSON.stringify(message));
    receive({ kind: 'ready', payload: { version: '2.1.0' } });
    await Promise.resolve();
    assert.ok(sent.some(m => m.name === 'x2357aiss.companionlog.roster.get'));
    receive({ kind: 'event', name: 'x2357aiss.companionlog.roster', payload: { roster: [
      { npc_id: 'sarah_morgan', display_name: 'Sarah Morgan', core_identity: 'Test profile', curated: true }
    ] } });
    const item = elements.get('roster-list').children.find(e => e.listeners.click);
    assert.ok(item); item.listeners.click();
    for (const name of ['memory', 'history']) {
      const request = sent.find(m => m.name === `x2357aiss.companionlog.${name}.get`);
      assert.deepEqual(request.payload, { command: request.name, npc_id: 'sarah_morgan', save_slot: 'Default' });
    }
    assert.equal(elements.get('empty-state').hidden, true);
    assert.equal(elements.get('npc-detail').hidden, false);
    assert.equal(elements.get('npc-name').textContent, 'Sarah Morgan');
    assert.match(elements.get('npc-relationship-summary').innerHTML, /Loading/);
    receive({ kind: 'event', name: 'x2357aiss.companionlog.memory', payload: {
      relationship_summary: 'Memory reply reached the original consumer', priority_memories: []
    } });
    assert.equal(elements.get('npc-relationship-summary').children[0].textContent,
      'Memory reply reached the original consumer');
    receive({ kind: 'event', name: 'x2357aiss.companionlog.memory', payload: {} });
    assert.equal(elements.get('npc-relationship-summary').children[0].textContent,
      'No history with this companion yet.');
    elements.get('refresh-btn').listeners.click();
    assert.match(elements.get('npc-relationship-summary').innerHTML, /Loading/,
      'Without a native memory reply the original consumer remains loading');
    receive({ kind: 'event', name: 'x2357aiss.companionlog.memory', payload: null });
    assert.equal(elements.get('npc-relationship-summary').children[0].textContent,
      'No history with this companion yet.');
  });
