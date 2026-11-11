# OSF UI 2.1 release preparation — November 11, 2026

**Status: rebuilding after a Developer-mode cold-launch failure; the candidate
identities and successful runs below are superseded pending the new build.
Version 2.1.0 has not shipped. No upload, tag or publication was performed.**

Install one edition. Both require SFSE, Address Library, Starfield 1.16.244.0,
and **OSF Settings 1.2.0 or a compatible newer release**. WebView2 requires
Evergreen; CEF includes its Chromium runtime. Coordinate publication with the
supported Settings dependency. This record does not qualify Settings itself.

## Changes since 2.0

- Separate WebView2 (recommended default) and bundled CEF editions with automatic
  backend selection and the same mod-facing interfaces.
- CEF reloads retain native replies and state. Renderer policy now survives
  overlapping browser objects with the same identifier. See the
  [reproduction and repair](../docs/reviews/cef-reload-2026-11-11.md).
- Improved first-frame presentation, Settings launcher handoff, repeated hotkeys,
  HUD/menu pointer input, browser recovery and early reply delivery.
- Refined CEF composition, damage copying, view isolation, bounded queues, GPU
  selection and local media seeking; reduced view-cache preparation work.
- Optional F11 diagnostics in Developer mode, including native capture/publication
  timing on WebView2. Developer mode defaults off and requires restart.
- Product version 2.1.0 retains native API 2.0, browser protocol 2 and documented
  legacy interfaces. World Screens and Browser TV remain separate releases.

The [Nexus fields and changelog](nexus/release-2.1-fields.txt) and
[listing description](nexus/description-2.1.bbcode.txt) are local publication drafts.
Performance evidence is workload-specific; no general CEF FPS or memory gain is claimed.

## Exact candidates

Both editions were built with `tools/package.ps1 -Backend both -Tag '' -RequireClean`
from clean source **`279cac814b16dc436da5b16b438c4199068af014`**. Their adjacent
manifests record `sourceDirty: false`, source revision and every payload hash.
Subsequent test-runner and documentation commits do not change these binaries.

| Archive | Bytes | SHA256 |
| --- | ---: | --- |
| `OSF-UI-v2.1.0-webview2.zip` | 1,583,529 | `56957C27EE40D1166718DA0FBD0A9112BEEE53CA31B506917446ED588550CDE4` |
| `OSF-UI-v2.1.0-cef.zip` | 185,286,645 | `04C38F0E639FCB2F9EDA59A3082C606C8C139AE8328A46E12BF4CDE510052495` |

Build receipts are in `build/release-2.1-20261111/final-packages/`.
CEF host DLL SHA256: `3F1214115428276E1E8D2A663872ED5ABF63DDF57B038607AA5BD4193488684D`.
The packager shares one verified backend-neutral broker across editions and still
requires byte-identical shared payloads. Backend exclusivity, all 55 Papyrus
natives, ZIP hashes and all ten negative package checks pass.

The final game dependency is production **OSFSettings 1.2.0-rc1**, source
`b786e935db46625ca9cbff9cdb65ebab40dc2f92`, `test_harness=n`, ZIP SHA256
`2C23A9D44F85EAA79249B38169A15361AFC9579E3222E1D73213C8AA166E83C3`.
Its exact manifest path is retained in
`build/release-2.1-20261111/settings-manifest-path.txt`. The test runner validates
each dependency against the validator from its recorded revision and saves that
validator and hash with the run.

## Verification

Evidence is local under `build/cef-reload-20261111/` and
`build/backend-validation/game/`. Native suites: **40 passed**. JavaScript SDK,
bridge, compatibility and diagnostics tests: **24 passed**. The real CEF helper
regression fails before the fix and passes initial load plus **ten consecutive
reloads** afterward, checking document identity, correlated replies and retained
state. Standalone GPU, storage, media and isolation/recovery checks also pass.

Final interactive runs used the exact archives above and the new Settings candidate:

| Run | Passed assertions | Failed | Captures |
| --- | ---: | ---: | ---: |
| `20261111-164514-993-cef-stock` | 99 | 0 | 10 |
| `20261111-164752-257-webview2-stock` | 97 | 0 | 10 |

Both cover native/Papyrus and legacy bridges, keyboard/mouse/wheel/select input,
relative input, focus and input isolation, reload/state, repeated menu opens,
HUD movement, pause ownership, save/load, helper termination/recovery, a 30-second
animation/bridge soak, clean shutdown and byte-for-byte browser storage restoration.
Both default-off F11 checks pass. The twenty game captures were visually reviewed:
browser markers and readable panels survive reload, load and recovery; Settings,
select popups, HUD and ordinary gameplay render correctly. Each run has zero
evidence warnings. Detailed receipts include observed outcomes, package hashes,
source snapshots and owned process identities.

CEF runtime run `20261111-165126-839-cef-stock-runtime` passed 71 assertions,
zero failures and 16 visually reviewed captures with the same dependency. It
checks actual GPU LUID, decoded video seeks, sibling isolation/recovery and browser
storage across two fresh games.

Cold-launch run `20261111-165412-946-cef-stock-developer` found that the initial
Developer-mode watcher refresh cancelled a pending Settings launcher request.
The runtime now defers draining watcher completions until the pending menu handoff
ends. The failed run is retained; new archives and repeat acceptance are required.

## Preserved failures and scope

- `20261111-162555-164-cef-stock-background`: unchanged October package reproduces
  the CEF reload failure. The standalone `before/` regression fails identically.
- `20261111-163728-364-cef-stock-background`: blocked before launch by a newer
  Settings validator applied to an unchanged old archive; validator revision pinning fixes it.
- `20261111-163849-503-cef-stock-background`: final CEF archive passes 70 assertions
  with the older October Settings dependency; useful background reload/recovery
  evidence, separately identified from the final dependency pairing.
- `20261111-164105-752-cef-stock`: functional checks and 120-second soak completed,
  but the final pause input was blocked by foreground loss. Not accepted as a
  passing run. The later complete final runs above are accepted.

Current game evidence is stock graphics on this machine's RTX 3080 at 3440×1440.
Physical controller, IME, Luma/HDR/frame generation and every historical third-party
consumer workflow were not requalified. The modern synthetic consumer and legacy
bridge are covered; this is not an exhaustive original-mod compatibility claim.

The [October 22 record](release-2.1-2026-10-22.md) preserves previous diagnostics,
first-frame findings and superseded candidates. The [October 19 record](release-2.1-2026-10-19.md)
also remains historical. Neither qualifies the new archives.

If runtime or packaged assets change before publication, rebuild both editions
from one clean revision and repeat acceptance against the new archives. At an
authorized release, publish matching author documentation and confirm the supported
Settings version is available. Uploads, Git pushes and release tags require a
separate release instruction.
