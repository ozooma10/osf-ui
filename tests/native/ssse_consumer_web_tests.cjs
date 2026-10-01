// Optional integration test: execute the installed, unchanged consumer in memory.
// Rendering is excluded; expose its existing closures instead of booting its DOM renderer.
// This never writes to the consumer. Set OSFUI_SSSE_ROOT to the original mod directory.
const { test } = require('node:test');
const assert = require('node:assert/strict');
const fs = require('node:fs');
const path = require('node:path');
const vm = require('node:vm');

test('installed Stock Exchange receives market state, emits trades and consumes chart results',
  { skip: !process.env.OSFUI_SSSE_ROOT }, async () => {
    const consumer = fs.readFileSync(path.join(process.env.OSFUI_SSSE_ROOT, 'SFSE/Plugins/OSFUI/views/x2357.ssse/exchange/main.js'), 'utf8');
    const boot = /  bind\(\);\s+showScreen\(\);\s+render\(\);\s+if \(demo\) window\.__ssse = .*?;\s*\}\)\(\);\s*$/;
    assert.match(consumer, boot, 'consumer boot changed; review the integration harness');
    const sent = [], timers = new Map(); let next = 1;
    const window = { osfui: { postMessage: text => sent.push(JSON.parse(text)) } };
    const document = { hidden: false, addEventListener() {}, getElementById: () => ({ addEventListener() {} }) };
    const context = vm.createContext({ window, document, console, performance,
      setTimeout: (fn, ms) => { const id = next++; timers.set(id, { fn, ms }); return id; },
      clearTimeout: id => timers.delete(id) });
    vm.runInContext(fs.readFileSync('src/Compat/V1/web/osfui.js', 'utf8'), context);
    context.osfui = window.osfui;
    vm.runInContext(consumer.replace(boot, '  bind(); window.__ssse = { S, U, act, req };\n})();'), context);
    const { S, act, req } = window.__ssse;
    const receive = message => window.osfui.onMessage(JSON.stringify(message));
    const market = Array(22).fill(0); market[0] = 1; market[8] = 250000;
    for (const [key, value] of Object.entries({ market, prices: [100, 200], stableIds: [3, 18], held: [10, 0], sectors: [1, 2] }))
      receive({ kind: 'state', mod: 'x2357.ssse', key, value });
    assert.deepEqual(Array.from(S.market), market);
    assert.deepEqual(Array.from(S.prices), [100, 200]);
    assert.deepEqual(Array.from(S.stableIds), [3, 18]);
    receive({ kind: 'ready', payload: { version: '2.0.0' } });
    await Promise.resolve();
    assert.equal(sent.at(-1).payload.action, 'refresh');
    act('buy', 3, 25); assert.deepEqual(sent.at(-1).payload.args, [3, 25]);
    act('sell', 3, 4); assert.deepEqual(sent.at(-1).payload.args, [3, 4]);
    act('sellOption', 18, 2, 1, 100, 1200);
    assert.deepEqual(sent.at(-1).payload.args, [18, 1, 100, 1200, 2]);
    for (const [name, args, value] of [
      ['candles', [3, 30], [95, 110, 90, 100]], ['intraday', [3], [99, 100]],
      ['closes', [3, 122, 15], [98, 100]], ['optionQuote', [3, 1, 100, 1200], 17],
      ['optionPositions', [15], [3, 1, 100, 1200, 2, 34, 17, 0]],
    ]) {
      const result = req(name, ...args);
      assert.equal(sent.at(-1).name, 'ui.papyrusRequest');
      assert.deepEqual(sent.at(-1).payload.args, args);
      assert.equal(sent.at(-1).payload.__osfuiV1TimeoutMs, 60000);
      receive({ kind: 'reply', id: sent.at(-1).id, payload: { __osfuiV1Reply: true, type: 'papyrus.result', payload: { value } } });
      assert.equal(JSON.stringify(await result), JSON.stringify(value));
    }
    const failed = req('unknown', 3);
    receive({ kind: 'error', id: sent.at(-1).id, payload: { code: 'bad-request', message: 'unknown' } });
    await assert.rejects(failed, error => error.code === 'bad-request');
    receive({ kind: 'event', name: 'data.reset', payload: { keys: ['market', 'prices', 'stableIds', 'held', 'sectors'] } });
    assert.equal(S.prices.length, 0);
    assert.equal(window.osfui.data.get('market'), undefined);
    receive({ kind: 'state', mod: 'x2357.ssse', key: 'prices', value: [120] });
    assert.equal(S.prices[0], 120);
  });
