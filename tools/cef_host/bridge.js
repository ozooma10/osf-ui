(() => {
  const post = window.__osfCefPost;
  const pending = [];
  const bridge = window.osfui = window.osfui || {};
  let handler = null;
  Object.defineProperty(bridge, 'onMessage', {
    configurable: true,
    get: () => handler,
    set: fn => {
      handler = fn;
      if (typeof fn === 'function') pending.splice(0).forEach(json => fn(json));
    },
  });
  bridge.postMessage = json => post(String(json));
  Object.defineProperty(window, '__osfCefReceive', { value: json => {
    try {
      const m = JSON.parse(json);
      if (m.kind === 'event' && m.name === 'ui.visibility' && typeof m.payload?.visible === 'boolean')
        bridge.performance?.setVisible(m.payload.visible);
      if (m.kind === 'event' && m.name === 'osfui.debug.performance') {
        bridge.performance?.toggle();
        return;
      }
      if (m.kind === 'event' && m.name === 'ui.visibility' && m.payload?.visible === false)
        document.activeElement?.blur?.();
    } catch (_) {}
    if (typeof handler === 'function') handler(json);
    else if (pending.length < 64) pending.push(json);
  }});
})();
