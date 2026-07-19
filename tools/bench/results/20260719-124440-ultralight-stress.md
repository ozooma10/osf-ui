# OSF UI renderer benchmark run — ultralight

- csv: `20260719-124440-ultralight-stress.csv`  log: `20260719-124440-ultralight-stress.log`
- renderer: **ultralight** · output: 1280x768 · view: osfui.bench/stress
- samples: 99 over 315s (mean interval 3.21s) — overlay open ~243s, closed ~72s

## Internal timing (Bench: channels, ms)

| channel | overlay | n | avg | p50 | p95 | p99 | max |
|---|---|---|---|---|---|---|---|
| frame | closed | 111345 | 0.545 | 0.147 | 2.179 | 3.251 | 100 |
| tick | closed | 111345 | 0.003 | 0.02 | 0.02 | 0.02 | 71.725 |
| produce | closed | 3664 | 0.173 | 0.06 | 0.11 | 2.943 | 150.721 |
| frame | open | 364724 | 0.666 | 0.092 | 1.971 | 2.261 | 100 |
| tick | open | 364724 | 0.013 | 0.02 | 0.02 | 0.433 | 1.955 |
| present | open | 145858 | 0.059 | 0.06 | 0.189 | 0.273 | 10.066 |
| produce | open | 8814 | 11.221 | 9.881 | 12.472 | 14.973 | 78.548 |

Overlay open: web frames produced **25.7/s**, compositor uploads **25.6/s** (over 243 s)

## Per scene (stress view)

Web engine throughput is the view's own rAF accounting; present/produce are the native channels for the same window.

| scene | cycles | web fps avg | web p95 frame (ms) | web long frames | present avg (ms) | native produced/s |
|---|---|---|---|---|---|---|
| idle | 2 | 43.6 | 33 | 82 | 0.043 | 4 |
| css-transform | 2 | 30.3 | 36 | 407 | 0.058 | 30.2 |
| paint | 2 | 13.9 | 79.5 | 557 | 0.055 | 13.9 |
| canvas | 2 | 30.8 | 37 | 604 | 0.066 | 27.4 |
| layout | 2 | 33.4 | 34 | 74 | 0.064 | 33.2 |
| fullscreen | 2 | 46.8 | 23 | 1 | 0.068 | 46.7 |

## External resources (per-second samples)

| metric | closed mean | closed max | open mean | open max |
|---|---|---|---|---|
| Starfield CPU % (of machine) | 16.04 | 33.7 | 11.25 | 19.63 |
| Starfield working set MB | 6321.54 | 8464.5 | 8283.39 | 8354.5 |
| Starfield private MB | 7907.8 | 9597.5 | 9459.21 | 9524.2 |
| Starfield GPU util % | 49.31 | 95.05 | 92.75 | 95.17 |
| Starfield GPU dedicated MB | 1042.09 | 1477 | 1470.35 | 1475.9 |
| WebView2 process count | 0 | 0 | 0 | 0 |
| WebView2 tree CPU % (of machine) | 0 | 0 | 0 | 0 |
| WebView2 tree working set MB | 0 | 0 | 0 | 0 |
| WebView2 tree private MB | 0 | 0 | 0 | 0 |
| WebView2 tree GPU util % | 0 | 0 | 0 | 0 |
| WebView2 tree GPU dedicated MB | 0 | 0 | 0 | 0 |

Combined open-overlay footprint (Starfield + WebView2 tree): CPU mean 11.25%, working set mean 8283 MB, GPU mean 92.75%

WebView2 orphans after game exit: 0
