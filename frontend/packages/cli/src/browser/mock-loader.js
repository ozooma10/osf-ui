const SOURCE = 'osfui-harness';
const harness = window.__osfuiHarness;
let invoke = null;

window.addEventListener('message', (event) => {
  if (event.origin !== location.origin || event.data?.source !== SOURCE ||
      event.data.kind !== 'tool-invoke') return;
  invoke?.(event.data.id, event.data.value);
});

try {
  const mod = harness.meta.mockUrl
    ? await import(/* @vite-ignore */ harness.meta.mockUrl)
    : {};
  await mod.install?.({
    registerTools(tools, onInvoke) {
      invoke = typeof onInvoke === 'function' ? onInvoke : null;
      parent.postMessage({ source: SOURCE, kind: 'tools', tools }, location.origin);
    },
  });
  parent.postMessage({ source: SOURCE, kind: 'mock-status', ok: true }, location.origin);
} catch (error) {
  parent.postMessage({
    source: SOURCE,
    kind: 'mock-status',
    ok: false,
    message: error instanceof Error ? error.message : String(error),
  }, location.origin);
}
