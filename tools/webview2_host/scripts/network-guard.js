(() => {
					for (const name of ['WebSocket', 'RTCPeerConnection',
							'webkitRTCPeerConnection', 'WebTransport',
							'Worker', 'SharedWorker']) {
						try {
							Object.defineProperty(window, name,
								{ value: undefined, writable: false, configurable: false });
						} catch (_) {}
					}
				})();
