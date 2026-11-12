# OSF UI 2.1 release preparation — updated November 12, 2026

**Status: the November 12 save-fix2 candidates supersede the earlier core archives
for saving with an overlay open. Version 2.1.0 has not shipped. No upload, tag or
publication was performed. Final publication still requires a clean source build.**

Install one edition. Both require SFSE, Address Library, Starfield 1.16.244.0,
and **OSF Settings 1.2.0 or a compatible newer release**. WebView2 requires
Evergreen; CEF includes its Chromium runtime. Coordinate publication with the
supported Settings dependency. This record does not qualify Settings itself.

## November 12 save repair

The overlay focus menu incorrectly contributed to the engine's save-blocking
counter. Allowing saves through that menu then exposed a second bug: a loaded
save could restore UI's temporary input lock after its panel had closed.

The repair sets the verified non-blocking menu flag, suspends only UI's own input
layer during save serialization, and reconciles that layer after world load.
Ordinary save events and the separately verified `Game.RequestSave` pump path are
both covered; the latter hook leaves Animation's existing inner save hook intact.
Failure to verify the hook disables input-capturing menus instead of installing
an unverified patch. The public API and Papyrus contracts are unchanged.

Both updated production editions are in `dist/`, with adjacent hash manifests:

| Current test candidate | SHA256 |
| --- | --- |
| `OSF-UI-v2.1.0-save-fix2-webview2.zip` | `7ABBDB6BFA33E9616B3EAD399569B0D70B6258DA5423CC8F3A99BD489AFD5458` |
| `OSF-UI-v2.1.0-save-fix2-cef.zip` | `00EDD4169A4D896E4AE2B2456DEB12AA04AE56B609B6003D0F3EAD48E48BE76D` |

WebView2 core DLL: `0A24B882431A2C19D9620C3D90D4B451E38A592A1D78B1E854BF318114F2D87C`.
CEF core DLL: `1378ACE7DC7B28F0F3B29711CA884A7AD9E6F3A9751157BDE284B8740BDA90A6`.
These were built from `a3975f0becbca4884474161649435ab4dbbd6a8d` plus local
repair changes; manifests record `sourceDirty: true`. The older clean-source
archives below do **not** contain this fix. Build clean final
archives and qualify those exact bytes before publishing.

All 76 native test cases pass, including production control-layer tests for
nested save guards, close during save, late completion, restored old masks and
preserving other owners. Both archives pass payload and 55 Papyrus signature
validation. Both editions also pass Animation's maintained 69 live cases with
unchanged production Settings 1.2.0 RC8. The production WebView2 consumer run passes
54 AISS/Seduce/Autonomous checks and an active encounter save with its panel open;
loaded controls and apparel match the pre-scene baseline. These runs live in the
sibling Animation repository's `build/release-followup-20261112/` and are indexed
in its `docs/RELEASE_2.0.0_ACCEPTANCE.md`.

The permanent packaged UI regression now checks quicksave and `Game.RequestSave`
with the input-capturing panel still open, loads each exact file and compares
fresh native control masks against the baseline. Browser storage and normal
Settings files are isolated and restored by hash. The final runs each passed
84 assertions with zero failures/evidence warnings:

- `build/backend-validation/game/20261112-173946-304-webview2-stock-background`
- `build/backend-validation/game/20261112-174124-084-cef-stock-background`

The exact WebView2 payload and production Settings RC8 are now installed in the
normal MO2 mod folders; originals and the per-file deployment receipt are retained
under Animation's `build/release-followup-20261112/`. The Default profile and saved
Settings values are unchanged. Physical controller input,
ordinary dialogue/AI workflows, pre-upgrade saves and longer play remain hands-on
acceptance, as do the graphics/hardware combinations listed below.

## Changes since 2.0

- Separate WebView2 (recommended default) and bundled CEF editions with automatic
  backend selection and the same mod-facing interfaces.
- CEF reloads retain native replies and state. Renderer policy now survives
  overlapping browser objects with the same identifier. See the
  [reproduction and repair](../docs/reviews/cef-reload-2026-11-11.md).
- Improved first-frame presentation, Settings launcher handoff, repeated hotkeys,
  HUD/menu pointer input, browser recovery and early reply delivery.
- Prevented Developer-mode file refresh from cancelling a pending cold launcher handoff.
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

The following November 11 archives and runs are historical. Use the save-fix2
candidates above for current testing; do not publish these older unpatched bytes.

Both editions were built with `tools/package.ps1 -Backend both -Tag '' -RequireClean`
from clean source **`3b91286925a68f226ed5983597a7b8fee256d6b9`**. Their adjacent
manifests record `sourceDirty: false`, source revision and every payload hash.
Subsequent test-runner and documentation commits do not change these binaries.

| Archive | Bytes | SHA256 |
| --- | ---: | --- |
| `OSF-UI-v2.1.0-webview2.zip` | 1,583,546 | `312A543101F847C670F464A543849593DB41ABE255A14F509E5DF7F55AC9552B` |
| `OSF-UI-v2.1.0-cef.zip` | 185,286,674 | `5C0811F3C9D007709F62E9F9866FA74A305D5580D85FF23A56CA4116C6713A13` |

The exact ZIPs, checksum files and manifests are ready in `dist/`, verified after
copying. Build receipts are in `build/release-2.1-20261111/qualified-packages/`.
Previous `dist` candidates are preserved in `build/release-2.1-20261111/previous-dist/`.
CEF host DLL SHA256: `3F1214115428276E1E8D2A663872ED5ABF63DDF57B038607AA5BD4193488684D`.
The packager shares one verified backend-neutral broker across editions and still
requires byte-identical shared payloads. Backend exclusivity, all 55 Papyrus
natives, ZIP hashes and all ten negative package checks pass.

The final dependency is production **OSF Settings 1.2.0**, candidate **RC8**,
source `ce14c0aa9c985c1c11d33972ad5e023329e32f8d`, `test_harness=n`.
`OSFSettings-1.2.0-rc8.zip` has SHA256
`DC5306CB79575892A54DE94F730A4869184B301E707C448297A2A3BD6A664C22`.
It is preserved under
`C:/Modding/Starfield/Artifacts/OSF Settings/1.2.0-followup-20261111/candidate-final/`.
Its manifest path is recorded in
`build/release-2.1-20261111/settings-final-manifest-path.txt`.

RC8's complete archive and all 16 payload files are byte-identical to RC7, which
passed the final paired game checks below. The loaded Settings DLL matches the
final archive: `A963A29DB461354B77D7AB31AEC72FE86CE44D677A48278CC43A9DDF3F2D47C0`.
The November 12 reconciliation rechecked both Settings archives and both UI ZIPs
against their manifests and the paired-run receipts. No runtime or package changes
were needed; `settings-final-verification.json` in the release build directory
records this check.

The earlier seven-run UI matrix used **Settings RC1**, source
`b786e935db46625ca9cbff9cdb65ebab40dc2f92`, ZIP SHA256
`2C23A9D44F85EAA79249B38169A15361AFC9579E3222E1D73213C8AA166E83C3`.
Its historical manifest path remains in `settings-manifest-path.txt`; it is not
relabelled as an RC8 test. Each run retains its revision-pinned Settings validator.

### Final dependency pairing

These later runs used the exact UI archives above and production Settings RC7
(the same archive bytes as final RC8):

| Run | Passed assertions | Failed | Captures |
| --- | ---: | ---: | ---: |
| `20261111-202611-783-cef-stock` | 119 | 0 | 14 |
| `20261111-203048-990-webview2-stock` | 117 | 0 | 14 |

Both passed the cold Settings launcher handoff, native/Papyrus and legacy bridge,
keyboard/mouse input, reload, save/load, browser recovery and clean shutdown.
Both have zero evidence warnings and completed temporary-profile cleanup; browser
storage was restored byte-for-byte. The Settings acceptance record reports visual
review of all 28 captures. Preserved `ui-cef/` and `ui-webview2/` reports, candidate
identities and events are under the same external artifact root above.

Settings' `RELEASE_RECORD.md` records its own production acceptance and boundaries:
12 supported runtime cases passed; failed-load injection is blocked because it
also aborts without Settings. Character Studio's separate harness smoke is blocked
by an existing consumer API/schema mismatch. Neither is evidence of an OSF UI
regression. Physical-controller coverage remains unverified. Preserve those limits
when publishing; this record does not claim that every Settings/harness case passed.

## Verification

Evidence is local under `build/cef-reload-20261111/` and
`build/backend-validation/game/`. Native suites: **40 passed** in `native-final.log`.
The new shared-runtime launcher regression has 43 checks. JavaScript SDK, bridge,
compatibility and diagnostics tests: **24 passed**. The real CEF helper regression
fails before the fix and passes initial load plus **ten consecutive reloads**
afterward, checking document identity, correlated replies and retained state.
Standalone GPU, storage, media and isolation/recovery checks also pass. The helper
bytes in the final package match those desktop checks.

The original UI qualification runs use the exact `3b912869` archives above and
Settings RC1. The final RC7/RC8 dependency pairing is recorded separately above:

| Run | Coverage | Passed assertions | Failed | Captures |
| --- | --- | ---: | ---: | ---: |
| `20261111-170644-359-cef-stock` | Full interactive | 99 | 0 | 10 |
| `20261111-170855-364-webview2-stock` | Full interactive | 97 | 0 | 10 |
| `20261111-170148-719-cef-stock-developer` | Cold Settings launch; F11 sampling on/off | 22 | 0 | 4 |
| `20261111-170324-796-cef-stock-developer` | Fresh restart with persisted Developer mode off | 19 | 0 | 1 |
| `20261111-170430-297-webview2-stock-developer` | Cold Settings launch; F11 including native timing | 22 | 0 | 4 |
| `20261111-170551-902-webview2-stock-developer` | Fresh restart with persisted Developer mode off | 19 | 0 | 1 |
| `20261111-171056-471-cef-stock-runtime` | GPU, media, view isolation and storage across two games | 71 | 0 | 16 |

The interactive runs cover native/Papyrus and legacy bridges, keyboard/mouse/wheel/
select input, relative input, focus and input isolation, reload/state, repeated
menu opens, HUD movement, pause ownership, save/load, helper termination/recovery,
a 30-second animation/bridge soak, clean shutdown and byte-for-byte browser storage
restoration. Both default-off F11 checks pass. The cold-launch runs begin without
a browser helper and use the visible Settings launcher through guarded keyboard
input; Settings closes, the focus menu takes ownership and browser pixels appear.
The Developer runs verify actual samples, removal of the sampler on hide and the
absence of the diagnostics API after an off-setting restart.

All **46 captures** from those seven runs were visually reviewed, with hashes saved
in each `visual-review.json`. Browser markers and readable panels survive reload,
load and recovery; Settings, select popups, HUD, diagnostics and ordinary gameplay
render correctly. These runs have zero evidence warnings. Receipts include observed
outcomes, package hashes, source snapshots and owned process identities.

The CEF runtime run matches Chromium's GPU LUID to the game adapter, checks
decoded colors after video seeks, observes distinct healthy-HUD pixels during
sibling slow load, failed navigation, missed reveal and renderer termination,
and verifies automatic recovery. localStorage, IndexedDB and a persistent cookie
survive normal game/helper shutdown and a fresh game. The retained 20-second VP8
fixture has SHA256 `68209FE14091FE6427FD1686442F2D66838B33FDB02822A020FAD872010C4BEA`.

Total: **349 passed assertions, zero failed assertions and zero evidence warnings**.
The later final-dependency pair adds **236 passed assertions**, for **585** across
both matrices; this combined count does not turn RC1-only checks into RC8 runs.
`build/release-2.1-20261111/acceptance-summary.json` records counts and cleanup for
all seven runs. Every registered temporary mod/profile was removed. All owned
games/helpers exited; original browser storage was restored by hash. The normal
installed OSF UI's 14 files remain byte-identical to the pre-test snapshot, and
the normal XMake configuration is restored to `releasedbg/webview2`.

## Preserved failures and scope

- `20261111-162555-164-cef-stock-background`: unchanged October package reproduces
  the CEF reload failure. The standalone `before/` regression fails identically.
- `20261111-163728-364-cef-stock-background`: blocked before launch by a newer
  Settings validator applied to an unchanged old archive; validator revision pinning fixes it.
- `20261111-163849-503-cef-stock-background`: earlier `279cac81` CEF archive passes 70 assertions
  with the older October Settings dependency; useful background reload/recovery
  evidence, separately identified from the final dependency pairing.
- `20261111-164105-752-cef-stock`: functional checks and 120-second soak completed,
  but the final pause input was blocked by foreground loss. Not accepted as a
  passing run. The later complete final runs above are accepted.
- `20261111-164514-993-cef-stock`, `20261111-164752-257-webview2-stock` and
  `20261111-165126-839-cef-stock-runtime`: successful preliminary checks against
  the `279cac81` archives, retained in `build/release-2.1-20261111/final-packages/`.
  Those misleadingly named earlier candidates are superseded by `qualified-packages/`
  and the final hashes above. Their record is preserved in
  `build/release-2.1-20261111/pre-launcher-release-record.md`.
- `20261111-165412-946-cef-stock-developer`: failed cold launcher handoff because
  the initial file-watch refresh cancelled the pending menu. Fixed by `3b912869`;
  both final cold-launch runs above pass. See the repair review for the cause.

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
