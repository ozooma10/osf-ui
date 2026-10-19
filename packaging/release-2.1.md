# OSF UI 2.1 release record

Local release preparation only. Publication, tags, and uploads require separate
authorization. Public author documentation starts at [the authoring guide](../docs/authoring.md);
Nexus drafts are in [nexus](nexus/release-2.1-fields.txt).

## Production dependency

Both editions require OSF Settings 1.1.2 or a compatible newer release. Acceptance
uses this exact production artifact; the version label alone is insufficient:

| Field | Value |
| --- | --- |
| Archive | `OSFSettings-1.1.2-release.zip` |
| Build directory | `OSFSettings-1.1.2-release-20261013-190150-17b69514` |
| Source | `6de2876bab663f6a515e99dedf7ac294bbb9942a` |
| Configuration | `windows/x64/releasedbg; test_harness=n` |
| Archive SHA256 | `EF5183DCBD6A72D7871780FF10EA1FA09621054913AE31F8A7F8B2137788E44A` |
| Loaded DLL SHA256 | `1EC8BADE3AB66C86CBF3F0B82D1B37A998AB80FFF80DC6FD794084F463F091AD` |

The five SDK headers at that source match `sdk/vendor` after line-ending
normalization. This records a tested dependency, not the publication status of
any companion release.

## Final archives

Both editions were built from clean source
`b8b701fa7f0a6275325bb752145cbf101caf521c` with
`pwsh tools/package.ps1 -Backend both -Tag ''`. The files are in `dist`, each with
a `.sha256` sidecar and `.manifest.json` recording source and payload hashes.
Subsequent release-record edits do not change the packaged runtime.

| Archive | Bytes | SHA256 |
| --- | ---: | --- |
| `OSF-UI-v2.1.0-webview2.zip` | 1,437,529 | `CD4BB744923BBE86E98052797F329B6EBE3EEEAD392815F39CE8E352392452AD` |
| `OSF-UI-v2.1.0-cef.zip` | 185,175,427 | `8A6053F50AC44E71285063AFECFB07E266A561B5008159D738B2CFE58076BC42` |

Both passed package ownership, compiled backend/API identity, all 55 Papyrus
native signatures, ZIP payload hash verification, and common-file equality.
Neither contains PDBs. Both include the 2.1.0 schema and Settings 1.1.2 dependency.
Build evidence: `build/release-2.1-package.log`.

## Final-archive game acceptance — October 19, 2026

The [game suite](../tests/game/README.md) tested these exact ZIPs with the pinned
production Settings artifact above, private saves, and private Settings storage.

| Edition | Result | Local evidence under `build/backend-validation/game` |
| --- | --- | --- |
| WebView2 | 100 assertions passed, zero failures; 114 completed observations; 10 captures visually reviewed | `20261019-212157-179-webview2-stock` |
| CEF | 68 assertions passed, zero failures; 56 completed observations; 16 captures with decoded frame markers | `20261019-212526-736-cef-stock-runtime` |

WebView2 passed text/Backspace/Tab/Space, clicks, wheel, select popup, relative
dragging, focus return, captured-input isolation, repeated menu opens, HUD
movement, pause ownership, retained state/reload, native and legacy APIs, Papyrus
before/after save/load, helper termination/recovery, and a 120-second animation
and bridge soak. Settings opened with F10 and released input on close. The
fresh game PID was 20120; Evergreen reported version 155.0.4283.62.

CEF passed hardware ANGLE rendering with Chromium's GPU LUID matching the game,
HTTP 206 bounded/suffix ranges, and decoded VP8/WebM seeks to 14.25, 2.5, 9.25 and
18.25 seconds followed by resumed playback. Actual healthy-HUD pixels advanced
during a sibling slow load, renderer crash/recovery, failed navigation/retry and
missed reveal. A 622,625-character localStorage/IndexedDB payload and persistent
cookie survived normal shutdown and a fresh game (PIDs 58036 and 31184). CEF is
150.0.22 / Chromium 150.0.7871.256. Representative video, failure-isolation and
fresh-storage captures were also visually reviewed.

Both suites closed their helpers with exit code zero, preserved live Settings
hashes, and reported no evidence warnings or device/hook failure. The CEF profile
was restored byte-for-byte. Logs contain the deliberately injected helper,
renderer and navigation failures followed by the observed recoveries. No new
production defect was found. Per-run `candidate.json`, `events.jsonl`,
`capture-review.json`, logs and reports retain the exact evidence.

After game exit, all 13 final WebView2 package files were deployed and hash-checked
in the configured MO2 `OSF UI` mod; unrelated files were preserved. Replaced files
are backed up under `build/release-2.1-deployment-backup`; the deployment record is
`build/release-2.1-deployment.json`. XMake remains configured for WebView2.

The requested local preparation gates are complete; nothing was pushed, tagged,
uploaded or published. Acceptance is scoped to Starfield 1.16.244.0, stock graphics,
RTX 3080 and a 2560x1409 game client. Luma/HDR/frame generation, physical
controllers, IME, CEF interactive input in this final-archive pass, other codecs,
other hardware, and all historical third-party integrations remain outside this
release check. Performance comparisons do not promise a general CEF advantage.
