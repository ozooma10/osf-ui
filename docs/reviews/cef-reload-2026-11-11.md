# CEF reload repair — November 11, 2026

The October reload failure reproduces with the unchanged production package in
`build/backend-validation/game/20261111-162555-164-cef-stock-background/`.
The first HUD document passes its native bridge checks. After `location.reload()`,
the new document renders and sends reports, but its native echo times out and
retained state remains absent. The owned helper exits cleanly and browser storage
is restored by hash. This run is a failure, not release acceptance.

## Cause and fix

The renderer stored immutable view policy by `CefBrowser::GetIdentifier()` and
unconditionally erased it in `OnBrowserDestroyed`. CEF can create a replacement
browser object before destroying the old object with the same identifier. This
also occurs during same-URL reload with this bundled Chromium's RenderDocument
lifecycle. CEF's `cef_render_process_handler.h` documents overlapping browser
identifiers during navigation.

The replacement had already received its JavaScript bridge when destruction of
the old object erased its policy. Native replies arrived at the replacement's
renderer but were discarded by the missing-policy guard. Thus rendering and
outbound page messages continued while replies and state stopped arriving.

`tools/cef_host/Host.cpp` now counts browser objects owning each policy entry and
erases it only after the last owner is destroyed. The origin, main-frame and
remote-content checks remain in force. No public API, saved format or shared host
protocol changes.

## Real-helper regression

The new `--checks reload` mode uses the production CEF helper and shipped
JavaScript SDK with private views/profile and a real GPU frame consumer. It
requires a correlated echo to the current document and state revision 7 after
ten consecutive same-URL reloads; every replacement needs a distinct document ID.

Evidence under `build/cef-reload-20261111/`:

- `before/`: unchanged helper fails its first reload with the same echo timeout.
- `diagnostic-run/` and `diagnostic-run2/`: temporary logging proves replies reach
  the new frame and are rejected after its policy disappears. Diagnostic source
  is retained in `diagnostic/Host.instrumented.cpp`; these logs are not compiled
  into the release helper.
- `after/`: fixed production helper passes initial load plus all ten reloads,
  correct state and clean shutdown. `reloaded.png` was visually inspected.
- `runtime/`: GPU selection, fresh-process storage persistence, media seeking,
  and view isolation/recovery all pass with the fixed helper.
- `native.log`: all 40 native suites pass, including runtime lifecycle scenarios.
- `javascript.log`: 24 SDK, compatibility, CEF bridge and diagnostics tests pass.

The original defect used helper SHA256
`01481AD38849CAAB05CB471B737FFF79AA338AF45B12EF873F601CF8563CE82E`.
The fixed packaged helper is
`3F1214115428276E1E8D2A663872ED5ABF63DDF57B038607AA5BD4193488684D`.
The exact source/binary hashes and failed artifacts remain with each run.

## Release preparation

Two independent native build directories exposed a broker byte mismatch during
packaging. Both editions now ship the first verified backend-neutral broker,
as they already do for the shared timestamp-bearing Papyrus script. The shared
payload equality check remains mandatory. The failed attempt is retained in
`package.log` and `build/release-2.1-20261111/packages/`.

The first final-package game attempt stopped before launch when the sibling
Settings validator's allowlist changed to require new documentation files. The
pinned October production archive and checksum had not changed. The game runner
now saves and uses the validator from that archive's recorded source revision,
retaining its hash rather than applying a later checkout's contract. The blocked
run is `20261111-163728-364-cef-stock-background`.

Final archive identities and game acceptance are recorded in
[the release record](../../packaging/release-2.1.md). Older successful game runs do
not qualify new binaries. Nothing in these desktop checks establishes physical
controller, HDR/Luma, arbitrary codecs or every original consumer workflow.
