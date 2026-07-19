# OSF UI renderer benchmark run — webview2

- csv: `20260719-121952-webview2-stress.csv`  log: `20260719-121952-webview2-stress.log`
- renderer: **webview2** · output: 1280x768 · view: osfui.bench/stress
- samples: 145 over 470s (mean interval 3.27s) — overlay open ~382s, closed ~88s

## Internal timing (Bench: channels, ms)

| channel | overlay | n | avg | p50 | p95 | p99 | max |
|---|---|---|---|---|---|---|---|
| frame | closed | 147814 | 0.526 | 0.149 | 2.06 | 2.932 | 100 |
| tick | closed | 147814 | 0.002 | 0.02 | 0.02 | 0.02 | 83.044 |
| frame | open | 568104 | 0.675 | 0.084 | 2.056 | 2.523 | 100 |
| tick | open | 568104 | 0.001 | 0.02 | 0.02 | 0.02 | 0.207 |
| present | open | 227226 | 0.052 | 0.06 | 0.086 | 0.146 | 7.537 |

Overlay open: web frames produced **35.7/s**, compositor uploads **35.7/s** (over 383 s)

## Per scene (stress view)

Web engine throughput is the view's own rAF accounting; present/produce are the native channels for the same window.

| scene | cycles | web fps avg | web p95 frame (ms) | web long frames | present avg (ms) | native produced/s |
|---|---|---|---|---|---|---|
| idle | 4 | 119.9 | 8.4 | 0 | 0.052 | 4.3 |
| css-transform | 3 | 73.1 | 19.53 | 18 | 0.054 | 47.8 |
| paint | 3 | 51.8 | 63.97 | 695 | 0.051 | 28.3 |
| canvas | 3 | 100.6 | 19.43 | 12 | 0.053 | 47.7 |
| layout | 3 | 69.1 | 22.3 | 12 | 0.054 | 48 |
| fullscreen | 3 | 72.2 | 19.53 | 24 | 0.052 | 48 |

## External resources (per-second samples)

| metric | closed mean | closed max | open mean | open max |
|---|---|---|---|---|
| Starfield CPU % (of machine) | 16.94 | 30.43 | 9.4 | 14.1 |
| Starfield working set MB | 6507.04 | 8258.7 | 8123.59 | 8195.4 |
| Starfield private MB | 8128.92 | 9396.9 | 9282.71 | 9330.4 |
| Starfield GPU util % | 55.54 | 94.59 | 87.17 | 94.58 |
| Starfield GPU dedicated MB | 1081.59 | 1456.9 | 1443.72 | 1457.4 |
| WebView2 process count | 6.22 | 8 | 8 | 8 |
| WebView2 tree CPU % (of machine) | 0.09 | 2.04 | 3.64 | 6.75 |
| WebView2 tree working set MB | 346.18 | 571.2 | 575.31 | 619.2 |
| WebView2 tree private MB | 204.84 | 393.2 | 374.46 | 430.4 |
| WebView2 tree GPU util % | 0 | 0 | 0 | 0 |
| WebView2 tree GPU dedicated MB | 0 | 0 | 0 | 0 |

Combined open-overlay footprint (Starfield + WebView2 tree): CPU mean 13.04%, working set mean 8699 MB, GPU mean 87.17%

WebView2 orphans after game exit: 0
