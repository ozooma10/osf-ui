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
Both were built from clean implementation commit `9a49478a` into
`build/release-2.1/packages-9a49478a`. SHA-256:

| Archive | SHA-256 |
|---|---|
| WebView2 | `0C6C7E177D9E4EC17958BFBC390E7EAFF7F41EB8B05FA9183B1A0FAF87B363E3` |
| CEF | `F1209AEE556CBA378D177752014EB5E7702CEAAEBEC702DC173178715E99085A` |

Both contain the same OSFUI.dll:
`324C0A726E7C80ED007614BA86AD77A5A5713122105F2B392ECDC52AA958B233`.
Archive ownership, all 55 compiled Papyrus signatures, frozen ABI, helper/runtime
checksums and common-payload identity passed. Later test/documentation commits
do not change these implementation bytes. Prior 2.0.0 ZIPs remain historical.

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
- After the version change: 29/29 native and 10/10 bridge/consumer tests passed.
  Production Settings passed 24/24 native tests. Its first parallel build hit
  MSVC C1090/PDB RPC error 0x6BA; the serial retry passed. Both logs are retained.
- Exact final 2.1 archives paired with the production Settings candidate passed
  the complete stock game suite at 3840x2160, including 120-second soaks, normal
  helper exit and unchanged live Settings storage. Runs:
  `20261018-031934-407-cef-stock` and `20261018-032340-042-webview2-stock`.
  CEF passed 100 assertions; WebView2 passed 98; neither had a failed assertion.
  All ten captures per run were visually inspected: Settings, panel, select
  popup, input/reload, legacy, HUD, pause, load, recovery and final gameplay.
- Three production-pairing startup failures (`031543-614`, `031653-546`,
  `031819-872`) are preserved. The test consumer attempted `.value()` on a null
  JSON object while waiting for storage redirection. Initializing an empty object
  fixed the fixture before production Settings loaded; no shipped code changed.
- Pending: available real-mod workflows and mod-manager switch persistence.
  The four available originals were copied and hash-verified without changing
  their DLLs, scripts or HTML. Run `20261018-032738-811-webview2-stock` verified
  private storage in Preload but ended during game startup, before any consumer
  workflow was accepted. An unrelated `OSF Defeat Testing` session then launched.
  The runner now reserves the primary harness lock as well as its isolated
  checkout lock; a subsequent attempt was correctly blocked by that reservation.
  Resume only when the other task releases the desktop. Current real-mod support
  builds and passes syntax checks; its full review/switch path is not yet accepted.
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
pwsh tests/game/Test-Backend.ps1 -Backend webview2 -Interactive -Package dist/OSF-UI-v2.1.0-webview2.zip -SettingsManifest <production-manifest.json>
pwsh tests/game/Test-Backend.ps1 -Backend cef -Interactive -Package dist/OSF-UI-v2.1.0-cef.zip -SettingsManifest <production-manifest.json>
```

`-SettingsManifest` verifies/stages the exact production archive and redirects
storage through the owned MO2 VFS before plugin initialization. Without it the
runner uses instrumented Settings; that mode is not production pairing evidence.
Publishing, pushing and tagging require a separate user request.
