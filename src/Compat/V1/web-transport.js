// Injected after the native bridge only for views discovered in the legacy root.
// Bundled 1.x helpers cannot be replaced through the shared/osfui.js asset URL.
(() => {
  const bridge = window.osfui;
  const post = bridge.postMessage;
  const receiver = Object.getOwnPropertyDescriptor(bridge, 'onMessage');
  let handler = receiver.get();
  let greeted = false;

  const hello = () => {
    if (greeted) return;
    greeted = true;
    post(JSON.stringify({ kind: 'send', name: 'osfui.hello', payload: {} }));
  };

  bridge.postMessage = json => {
    let message;
    try { message = JSON.parse(json); } catch (_) { return post(json); }
    if (message && !Object.hasOwn(message, 'kind') && message.type === 'ui.command') {
      const payload = message.payload;
      if (payload && typeof payload === 'object' && !Array.isArray(payload)) {
        const request = Object.hasOwn(message, 'requestId');
        message = { kind: request ? 'request' : 'send', name: payload.command, payload,
          ...(request ? { id: message.requestId } : {}) };
        json = JSON.stringify(message);
      }
    }
    if (message && message.kind === 'send' && message.name === 'osfui.hello') return hello();
    return post(json);
  };

  const receive = json => {
    let message;
    try { message = JSON.parse(json); } catch (_) { return handler(json); }
    if (message) {
      switch (message.kind) {
        case 'ready': message = { type: 'runtime.ready', payload: message.payload }; break;
        case 'event': message = { type: message.name, payload: message.payload }; break;
        case 'state': message = { type: 'data.state', payload: {
          mod: message.mod, key: message.key, value: message.value } }; break;
        case 'reply':
          message = message.payload && message.payload.__osfuiV1Reply === true
            ? { type: message.payload.type, payload: message.payload.payload, requestId: message.id }
            : { type: 'ui.result', payload: message.payload, requestId: message.id };
          break;
        case 'error': message = { type: 'ui.error', payload: message.payload, requestId: message.id }; break;
        default: return handler(json);
      }
    }
    handler(JSON.stringify(message));
  };

  Object.defineProperty(bridge, 'onMessage', {
    configurable: true,
    get: () => handler,
    set: fn => {
      handler = fn;
      receiver.set(typeof fn === 'function' ? receive : fn);
      // Original helpers wait for runtime.ready without initiating a greeting.
      if (typeof fn === 'function') hello();
    },
  });
  if (typeof handler === 'function') bridge.onMessage = handler;
})();
