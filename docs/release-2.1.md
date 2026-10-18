# OSF UI 2.1 release qualification

Status: 2.1.0 release candidates are built; original-mod qualification remains
incomplete. Both rebuilt archives passed packaged acceptance with production
Settings. Three original fixtures are unavailable locally. Not published.
The two editions share native API
2.0, browser protocol 2, the frozen legacy bridge, and identical common payloads.
WebView2 remains the default; CEF is the bundled-runtime alternative. Install one.

## Release changes

- Separate, automatically selected WebView2 and CEF packages.
- CEF damage-aware texture copying with a full-copy fallback for missing damage,
  new surfaces, geometry changes and discontinuous capture counters.
- Consolidated CEF publication scheduling and reveal state, preserving bounded
  coalescing, backpressure and completion of borrowed texture copies.
- Preserve game-owned raw keyboard releases while an overlay captures input,
  preventing missed repeated hotkeys after closing a view.
- Retain the latest frame arriving during the Settings launcher handshake,
  preventing a transparent initial capture from hiding a fully painted static menu.
- Mixed menu/HUD/overlay benchmarks and corrected capture checks that tolerate
  display color conversion while detecting missing content and HUD trails.

The [same-PC measurements](cef-compositing-profile.md) do not establish a general
CEF CPU, memory or throughput improvement. Dirty copying reduced copied area by
about 1% in the measured mixed fixtures; full-canvas fixtures still copied the
whole surface. The slower 1440p three-view repetition is retained in the evidence.

## Candidate and dependency identity

Final names are `OSF-UI-v2.1.0-webview2.zip` and `OSF-UI-v2.1.0-cef.zip`.
Current candidates were built from clean implementation commit `f868ddff` into
`build/release-2.1/packages-frame-queue-fix`. SHA-256:

| Archive | SHA-256 |
|---|---|
| WebView2 | `3530DE9059BA865720620BD849207C0F0C67CEC8DDE5179EE1A8741E387B2B0E` |
| CEF | `D97E9B161FC08BBF7CD9D77FBB7248FD12E8D12021877B3FE4DE9E306688B161` |

Both contain the same OSFUI.dll:
`18EBF7800CACDA5C841176FE9B9FE9CEA5B4D0A90E435C4CD053E291AFD305B4`.
Archive ownership, all 55 compiled Papyrus signatures, frozen ABI, helper/runtime
checksums and common-payload identity passed. Later test/documentation commits
do not change these implementation bytes. Prior 2.0.0 ZIPs remain historical.

The first 2.1 candidates at `9a49478a` passed the game suite, but a subsequent
audit found the legacy native `GetPluginVersion` still hardcoded to 2.0.0. The
current fix uses the shared release constants without changing ABI slots or
protocol versions. The compiled-binary probe now compares that actual virtual
call with the PE version resource: the old DLL fails, the corrected DLL reports
2.1.0 and passes. Both version-query archives passed fresh game acceptance before the subsequent
input and frame-queue fixes. Those archives remain historical evidence.
The old archives and logs remain intact.

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
- Current offline bridge/consumer run: 16 tests passed, zero skipped, including
  the installed, unchanged Stock Exchange JavaScript. This does not execute its
  game VM or render its UI.
- Production Settings packaging: passed payload/checksum/production-marker checks.
- After the version change: 29/29 native and 10/10 bridge/consumer tests passed.
  Production Settings passed 24/24 native tests. Its first parallel build hit
  MSVC C1090/PDB RPC error 0x6BA; the serial retry passed. Both logs are retained.
- Initial 2.1 candidates from `9a49478a`, paired with production Settings, passed
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
- After the version-query fix: production builds, both archive checks and all
  29 native tests passed again. The first game attempt was blocked by the primary
  harness lock before launching a game (`game-cef-version-query-fix.log`). After
  the other tests paused, both version-query ZIPs passed the complete production Settings
  suite: `20261018-035844-454-webview2-stock` (99 assertions) and
  `20261018-040245-433-cef-stock` (101 assertions), zero failures. Each included a
  120-second soak and normal helper exit; live Settings hashes were unchanged.
  All ten captures per run were visually inspected, including final gameplay.
- Pending: available real-mod workflows and mod-manager switch persistence.
  The four available originals were copied and hash-verified without changing
  their DLLs, scripts or HTML. Run `20261018-032738-811-webview2-stock` verified
  private storage in Preload but ended during game startup, before any consumer
  workflow was accepted. An unrelated `OSF Defeat Testing` session then launched.
  The runner now reserves the primary harness lock as well as its isolated
  checkout lock; a subsequent attempt was correctly blocked by that reservation.
  The desktop reservation is now available. Current real-mod support builds and
  passes syntax checks; its full review/switch path is not yet accepted.
- Original-mod runs `20261018-040649-039-webview2-stock` and
  `20261018-040920-610-webview2-stock` stopped at the automatic Settings hotkey
  check. Both failure captures show AISS's initial text-entry prompt. A fresh
  manual review captured that prompt before any key input, dismissed it with
  Escape, then opened Settings normally with F10. The first hotkey-collision
  hypothesis was disproved; no production fix or default binding change was
  needed. Original-mod review now handles startup prompts explicitly. Shared
  Settings, Starcade and legacy overwrite storage remained unchanged.
- Missing original fixtures on this PC: DDC, Field.OS AEGIS and DevilzDad's Shop
  + Explorer. Their real-mod acceptance remains pending; fixture/unit tests do
  not establish game behavior. The user confirmed they may not be downloaded.
- Original-mod review `20261018-041212-161-webview2-stock` is **failed**, not
  release acceptance. Its inspected captures, Papyrus log, private values and
  `OSFUIRealModsDiagnostic.sfs` checkpoint are retained. SSSE rendered all five
  chart periods, bought/sold a share and bought an option; inventory observations
  confirmed credits 9900 → 9816 → 9880 → 9605. Starcade's warm reopen rendered,
  Great Serpent accepted keyboard input, and a 100-credit blackjack wager/refund
  changed actual inventory 9605 → 9505 → 9605. AISS search/profile navigation and
  Somatic setting callbacks/private F6 persistence were observed.
  Remaining findings: intermittent SSSE F7 reopening, no observed F9 opening,
  and Starcade's first Settings launch capturing input with no visible interface
  until explicit close/reopen. AISS retained its empty-profile placeholder and
  relationship-loading state. These need diagnosis/comparison before acceptance.
  Shared Settings, Starcade and legacy overwrite hashes remained unchanged;
  the helper exited normally. The diagnostic checkpoint is not an accepted
  edition-switch baseline.

Local evidence is retained under `build/release-2.1/`; failed attempts must remain
alongside accepted runs. No unrelated game session may be interrupted.

Further diagnosis on October 18: CEF run `20261018-045258-801-cef-stock`
also failed original-mod acceptance. Its 29 reviewed captures show a successful
first Starcade Settings launch, but F9 never reached either the direct Settings
provider observer or the legacy observer. A private F6 rebind reached both and
opened the original mod. An intermittent F7 reopen missed both observers too;
the next press reached both and opened the populated exchange. Native Somatic
edits/rebinding updated legacy callbacks and private JSON, but camera behavior
remained inconclusive in an occluded view. AISS search and Sarah Morgan's profile
rendered with the same empty-placeholder issue on CEF. The original stylesheet
sets `#empty-state` to `display: flex` while its script sets the HTML `hidden`
property; this is a consumer-side cause to verify, not a reason to alter its
installed files. Shared stores remained unchanged and the CEF helper exited
normally. These observations do not complete the edition-switch gate.

An earlier CEF attempt (`20261018-044809-218`) ran out of disk space before
launch. Its archive checksum still matched. NTFS compression of this task's
build object files recovered about 5.8 GiB without deleting failed evidence or
compressing runtime payloads. The runner now checks staging/evidence headroom
before extraction. Settings branch `osf/ui-21-hotkey-validation` retains the diagnostic tracing
commit and its revert. Its current source matches the original production
Settings candidate; tracing archives remain diagnostic evidence only.

## Input and first-presentation findings resolved

`78d588da` fixes raw keyboard packet validation: keyboard packets are 40 bytes on
this x64 build, smaller than `sizeof(RAWINPUT)` (48). The prior equality check
missed ownership tracking and swallowed a game-owned release after an overlay
opened. Actual-layout tests cover malformed packets and repeated capture cycles.
A traced Settings diagnostic observed 12/12 F7 opens in run
`20261018-051707-350-webview2-stock`; this was not the production pairing.

`f868ddff` retains one coalesced following frame while the prepared frame remains
pinned for the launcher handshake. Producer fences and slot-reuse protection
still apply. The native suite now passes 30/30 tests, including prepared/latest
frame ordering, cancellation and reuse. Both production helpers were
packaged with the corrected shared DLL; no timing experiments remain.

Run `20261018-053435-825-webview2-stock` uses the current WebView2 ZIP and original
production Settings. The first Starcade launcher handoff after opening/closing
Stock Exchange rendered its 26-game library immediately. Eight additional F7
open/close cycles succeeded. Starcade keyboard gameplay and a 100-credit wager/
refund changed actual inventory 9900 -> 9800 -> 9900. Stock Exchange TerraBrew buy/
sell changed inventory 9900 -> 9857 -> 9880, matching the UI. Its month/week/five-year
charts rendered; day/year captures were premature and are not accepted. Rapid
reopens also exposed a delayed original Papyrus market-guard backlog. Starcade's
balance label stayed stale after dealing despite the confirmed inventory debit.

All 45 captures were inspected and recorded, including navigation mistakes; this
run remains **failed** overall. AISS still showed its empty placeholder and
relationship-loading state. Somatic callbacks/rebind/private values worked, but
camera appearance was inconclusive. Subsequent fixture inspection found
`ImprovedCameraSF.esm` disabled: its invisible headgear was unavailable. The runner
now enables that original ESM in the private profile; the old camera evidence
cannot establish a consumer or framework defect. Default Starcade F9 remains
unresolved; native binding UI also lists F9 for QuickLoad, while a private F6
rebind opened the original mod. This is a suspected binding conflict, not a
proven cause or a production fix.

Shared Settings, Starcade and legacy overwrite hashes stayed unchanged. The
review shell crashed while serializing a FileInfo object from a save waiter;
the retained diagnostic checkpoint exists, but the game later disappeared and
normal exit was not established. The checkpoint is not an accepted edition-switch
baseline. Never use this failed run to satisfy the resume acceptance gate.

Both exact current packages passed the complete stock suite at 3840x2160 with
production Settings, a 120-second soak, unchanged live Settings hashes and clean
helper exit. CEF run `20261018-063040-666-cef-stock` passed 102 assertions; WebView2
run `20261018-063444-923-webview2-stock` passed 100. Neither had a failed assertion.
All ten captures per run were visually inspected and recorded with SHA-256 hashes.
These package checks are complete; original-mod and edition-switch gates remain open.

The strict desktop cold-open probe still reports a blank first helper capture in
retained legacy-fixture failures. That probe does not run the game-side prepared
frame queue. It remains useful diagnostic evidence; the queue tests and actual
first Starcade launcher capture establish the scope of the fix.

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

## Local resume

Use the existing candidates rather than rebuilding unchanged bytes. Once the
primary harness reservation and desktop are free:

```powershell
$settings = (Get-Content build/release-2.1/settings-manifest-path.txt -Raw).Trim()
$harness = 'C:\Modding\Starfield\OSF Test Harness UI Validation'
$packages = 'build/release-2.1/packages-frame-queue-fix'
pwsh tests/game/Test-Backend.ps1 -Backend webview2 -Interactive -HarnessRoot $harness -SettingsManifest $settings -Package "$packages/OSF-UI-v2.1.0-webview2.zip"
pwsh tests/game/Test-Backend.ps1 -Backend cef -Interactive -HarnessRoot $harness -SettingsManifest $settings -Package "$packages/OSF-UI-v2.1.0-cef.zip"
pwsh tests/game/Test-Backend.ps1 -Backend webview2 -Interactive -RealMods -HarnessRoot $harness -SettingsManifest $settings -Package "$packages/OSF-UI-v2.1.0-webview2.zip"
```

Follow [the real-mod review procedure](../tests/game/README.md#original-consumer-review),
inspect all captures, then run CEF `-RealMods -ResumeRun <accepted-WebView2-run>`
to check the same private save and persisted settings across the edition switch.
Do not mark the four-mod review or the three unavailable original fixtures as
accepted until their actual workflows have been observed.
