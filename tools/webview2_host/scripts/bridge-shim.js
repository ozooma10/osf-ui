(() => {
						const bridge = window.osfui = window.osfui || {};
						const pending = [];
						let onMessage = typeof bridge.onMessage === 'function' ?
							bridge.onMessage : null;
						Object.defineProperty(bridge, 'onMessage', {
							configurable: true,
							get: () => onMessage,
							set: (fn) => {
								onMessage = fn;
								if (typeof fn === 'function')
									pending.splice(0).forEach((json) => fn(json));
							}
						});
						bridge.postMessage = (json) => chrome.webview.postMessage(String(json));
						if (!window.chrome || !chrome.webview) return;
						bridge.nativePerformance = (action, session, scrolling, scrollAgeMs) =>
							chrome.webview.postMessage('__osfuiPerformance:' + JSON.stringify({ action, session, scrolling, scrollAgeMs }));
						chrome.webview.addEventListener('message', (event) => {
							const json = typeof event.data === 'string' ?
								event.data : JSON.stringify(event.data);
							try {
								const m = JSON.parse(json);
								if (m && m.kind === 'osfui.nativePerformance') {
									bridge.performance?.updateNative(m.session, m.payload);
									return;
								}
								const visibility = m && m.kind === 'event' && m.name === 'ui.visibility';
								if (visibility && typeof m.payload?.visible === 'boolean') bridge.performance?.setVisible(m.payload.visible);
								if (m && m.kind === 'event' && m.name === 'osfui.debug.performance') {
									bridge.performance?.toggle();
									return;
								}
								if (visibility && m.payload && m.payload.visible === false) {
									const el = document.activeElement;
									if (el && el !== document.body && typeof el.blur === 'function') el.blur();
								}
							} catch (_) {}
							if (typeof onMessage === 'function') onMessage(json);
							else pending.push(json);
						});
					})();
