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
						const nativePopupControl = (event) => {
							const path = typeof event.composedPath === 'function' ?
								event.composedPath() : [event.target];
							for (const el of path) {
								if (el instanceof HTMLSelectElement && !el.disabled) return el;
								if (!(el instanceof HTMLInputElement) || el.disabled) continue;
								if (el.list) return el;
								if (['color', 'date', 'datetime-local', 'month', 'time', 'week']
									.includes(el.type)) return el;
							}
							return null;
						};
						const nativePopupMessage = '__osfuiNativePopup:';
						if (window === window.top) {
							window.addEventListener('message', (event) => {
								if (event.data !== nativePopupMessage + '0' &&
									event.data !== nativePopupMessage + '1') return;
								if (window.chrome && chrome.webview)
									chrome.webview.postMessage(event.data);
							});
						}
						let nativePopup = null;
						const reportNativePopup = (open) => {
							const message = nativePopupMessage + (open ? '1' : '0');
							if (window !== window.top) {
								window.top.postMessage(message, '*');
							} else if (window.chrome && chrome.webview) {
								chrome.webview.postMessage(message);
							}
						};
						const closeNativePopup = (event) => {
							if (!nativePopup) return;
							if (event && nativePopupControl(event) !== nativePopup) return;
							nativePopup = null;
							reportNativePopup(false);
						};
						document.addEventListener('pointerdown', (event) => {
							const control = nativePopupControl(event);
							if (control) {
								nativePopup = control;
								reportNativePopup(true);
							} else {
								closeNativePopup();
							}
						}, true);
						document.addEventListener('keydown', (event) => {
							const control = nativePopupControl(event);
							if (control && (event.key === 'Enter' || event.key === ' ' ||
								(event.altKey && event.key === 'ArrowDown'))) {
								nativePopup = control;
								reportNativePopup(true);
							} else if (event.key === 'Escape') {
								closeNativePopup();
							}
						}, true);
						document.addEventListener('change', closeNativePopup, true);
						document.addEventListener('input', (event) => {
							if (nativePopup instanceof HTMLInputElement && nativePopup.list)
								closeNativePopup(event);
						}, true);
						document.addEventListener('blur', closeNativePopup, true);

						const VK_KEYS = {
							0x08: ['Backspace', 'Backspace'], 0x09: ['Tab', 'Tab'],
							0x0D: ['Enter', 'Enter'], 0x1B: ['Escape', 'Escape'],
							0x20: [' ', 'Space'],
							0x21: ['PageUp', 'PageUp'], 0x22: ['PageDown', 'PageDown'],
							0x23: ['End', 'End'], 0x24: ['Home', 'Home'],
							0x25: ['ArrowLeft', 'ArrowLeft'], 0x26: ['ArrowUp', 'ArrowUp'],
							0x27: ['ArrowRight', 'ArrowRight'], 0x28: ['ArrowDown', 'ArrowDown'],
							0x2D: ['Insert', 'Insert'], 0x2E: ['Delete', 'Delete'],
							0x10: ['Shift', 'ShiftLeft'], 0x11: ['Control', 'ControlLeft'],
							0x12: ['Alt', 'AltLeft'],
							0xA0: ['Shift', 'ShiftLeft'], 0xA1: ['Shift', 'ShiftRight'],
							0xA2: ['Control', 'ControlLeft'], 0xA3: ['Control', 'ControlRight'],
							0xA4: ['Alt', 'AltLeft'], 0xA5: ['Alt', 'AltRight']
						};
						const MOD_VKS = {
							0x10: 'shift', 0xA0: 'shift', 0xA1: 'shift',
							0x11: 'ctrl', 0xA2: 'ctrl', 0xA3: 'ctrl',
							0x12: 'alt', 0xA4: 'alt', 0xA5: 'alt'
						};
						const heldMods = { shift: false, ctrl: false, alt: false };
						const synthesizeKey = (vk, down) => {
							const mod = MOD_VKS[vk];
							if (mod) heldMods[mod] = down;
							let key, code;
							const named = VK_KEYS[vk];
							if (named) {
								key = named[0]; code = named[1];
							} else if (vk >= 0x41 && vk <= 0x5A) {
								const ch = String.fromCharCode(vk);
								key = heldMods.shift ? ch : ch.toLowerCase();
								code = 'Key' + ch;
							} else if (vk >= 0x30 && vk <= 0x39) {
								key = String.fromCharCode(vk);
								code = 'Digit' + key;
							} else if (vk >= 0x70 && vk <= 0x87) {
								key = 'F' + (vk - 0x6F); code = key;
							} else {
								return;  // unmapped VK: nothing sensible to synthesize
							}
							let doc = document;
							let target = doc.activeElement || doc.body;
							while (target && target.tagName === 'IFRAME') {
								let inner = null;
								try { inner = target.contentDocument; } catch (_) {}
								if (!inner) break;
								doc = inner;
								target = doc.activeElement || doc.body;
							}
							if (target && target.tagName === 'IFRAME') {
								if (target.contentWindow)
									target.contentWindow.postMessage(
										{ __osfuiKeyRelay: { vk, down } }, '*');
								return;
							}
							if (!target) target = document.body;
							const ev = new KeyboardEvent(down ? 'keydown' : 'keyup', {
								key, code, bubbles: true, cancelable: true, composed: true,
								shiftKey: heldMods.shift, ctrlKey: heldMods.ctrl,
								altKey: heldMods.alt
							});
							Object.defineProperty(ev, 'keyCode', { get: () => vk });
							Object.defineProperty(ev, 'which', { get: () => vk });
							target.dispatchEvent(ev);
							if (down && !ev.defaultPrevented && target.tagName === 'INPUT' &&
								target.type === 'range' && key.startsWith('Arrow')) {
								const step = Number(target.step) || 1;
								const dir = (key === 'ArrowRight' || key === 'ArrowUp') ? 1 : -1;
								const min = target.min !== '' ? Number(target.min) : 0;
								const max = target.max !== '' ? Number(target.max) : 100;
								const cur = Number(target.value) || 0;
								const next = Math.min(max, Math.max(min, cur + dir * step));
								if (next !== cur) {
									target.value = String(next);
									target.dispatchEvent(new Event('input', { bubbles: true }));
									target.dispatchEvent(new Event('change', { bubbles: true }));
								}
							}
						};

						window.addEventListener('message', (event) => {
							if (event.source !== window.parent) return;
							const k = event.data && typeof event.data === 'object' ?
								event.data.__osfuiKeyRelay : null;
							if (k) synthesizeKey(k.vk | 0, !!k.down);
						});

						if (!window.chrome || !chrome.webview) return;
						chrome.webview.addEventListener('message', (event) => {
							if (event.data && typeof event.data === 'object' &&
								event.data.__osfuiKey) {
								const k = event.data.__osfuiKey;
								synthesizeKey(k.vk | 0, !!k.down);
								return;
							}
							const json = typeof event.data === 'string' ?
								event.data : JSON.stringify(event.data);
							try {
								const m = JSON.parse(json);
								const visibility = m && (m.type === 'ui.visibility' ||
									(m.kind === 'event' && m.name === 'ui.visibility'));
								if (visibility && m.payload && m.payload.visible === false) {
									const el = document.activeElement;
									if (el && el !== document.body && typeof el.blur === 'function') el.blur();
								}
							} catch (_) {}
							if (typeof onMessage === 'function') onMessage(json);
							else pending.push(json);
						});
					})();
