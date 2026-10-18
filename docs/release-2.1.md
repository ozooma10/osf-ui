# OSF UI 2.1 release qualification

Status: preparation in progress; not published. The two editions share native API
2.0, browser protocol 2, the frozen legacy bridge, and identical common payloads.
WebView2 remains the default; CEF is the bundled-runtime alternative. Install one.

## Release changes

- Separate, automatically selected WebView2 and CEF packages.
- CEF damage-aware texture copying with a full-copy fallback for missing damage,
  new surfaces, geometry changes and discontinuous capture counters.
- Consolidated CEF publication scheduling and reveal state, preserving bounded
  coalescing, backpressure and completion of borrowed texture copies.
- Mixed menu/HUD/overlay benchmarks and corrected capture checks that tolerate
  display color conversion while detecting missing content and HUD trails.

The [same-PC measurements](cef-compositing-profile.md) do not establish a general
CEF CPU, memory or throughput improvement. Dirty copying reduced copied area by
about 1% in the measured mixed fixtures; full-canvas fixtures still copied the
whole surface. The slower 1440p three-view repetition is retained in the evidence.

## Candidate and dependency identity

Final names are `OSF-UI-v2.1.0-webview2.zip` and `OSF-UI-v2.1.0-cef.zip`.
Archive names alone do not establish validation; record hashes and the clean
implementation commit after building. The previously accepted 2.0.0 handoff ZIPs
remain historical evidence, not final 2.1 binaries.

The production Settings candidate is 1.1.2 at
`1a857dbab19815b0a86c47b217cb8ac1146c8ce4`, built with `test_harness=n`.
Its five public SDK headers match `sdk/vendor` byte for byte. Local ZIP:
`OSFSettings-1.1.2-osfui-2.1.zip`, SHA-256
`B544DD1D29B5847E214FDE9F4E7469879552A22A8E72459BC578748702E0ED92`.
This is a local production candidate, not a claim that this artifact is published.

## Validation ledger

- Completed before the version change: production builds, 29 native tests,
  nine bridge tests, 42 desktop validation sessions and both exact handoff ZIPs
  in stock Starfield. See [the final handoff report](cef-compositing-profile.md#final-package-acceptance).
- Current offline bridge/consumer run: 10 tests passed, zero skipped, including
  the installed, unchanged Stock Exchange JavaScript. This does not execute its
  game VM or render its UI.
- Production Settings packaging: passed payload/checksum/production-marker checks.
- Pending: production Settings in-game pairing, available real-mod workflows,
  mod-manager upgrade/switch persistence, and both exact final 2.1 archives in-game.
- Missing original fixtures on this PC: DDC, Field.OS AEGIS and DevilzDad's Shop
  + Explorer. Their real-mod acceptance remains pending; fixture/unit tests do
  not establish game behavior. The user confirmed they may not be downloaded.

Local evidence is retained under `build/release-2.1/`; failed attempts must remain
alongside accepted runs. No unrelated game session may be interrupted.

Luma/HDR/frame generation, physical controller/IME, alternate GPUs, a machine
without Evergreen and long-session performance remain outside current acceptance.

## Release commands

```powershell
pwsh tools/package.ps1 -Backend both -Tag '' -NoPdb
pwsh tests/game/Test-Backend.ps1 -Backend webview2 -Interactive -Package dist/OSF-UI-v2.1.0-webview2.zip
pwsh tests/game/Test-Backend.ps1 -Backend cef -Interactive -Package dist/OSF-UI-v2.1.0-cef.zip
```

The default game runner uses instrumented Settings for private storage. A run
using that default must not be described as production Settings acceptance.
Publishing, pushing and tagging require a separate user request.
