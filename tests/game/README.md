# Packaged browser acceptance

This suite drives the production OSFUI.dll through a separate SFSE consumer of
the public SDK. No instrumentation is added to the shipped core or browser hosts.
It uses the sibling OSF Test Harness for owned process identity, input, native
console dispatch, private saves/profiles, WGC screenshots, and reports. Read that
repository's AGENTS.md and BACKGROUND.md before running. Interactive tests need
explicit user authorization and an idle, unlocked desktop.

```powershell
pwsh tests/game/Build.ps1
pwsh tests/game/Test-Backend.ps1 -Backend webview2 -Interactive
pwsh tests/game/Test-Backend.ps1 -Backend cef -Interactive
```

`-Package` selects an exact ZIP; the default is the corresponding local RC in
`dist`. For release qualification, pass `-SettingsManifest` with the production
Settings archive manifest. Its archive, payload and production markers are
validated, and the complete exact archive is staged. The acceptance consumer
waits in SFSE Preload for `private_storage.py` to connect to the owned MO2 VFS and
map normal Documents Settings storage to a private copy. PID, process creation,
session marker and mapped reads are verified before production Settings loads.
The runner compares live storage hashes before and after the session. The public
Settings API, persisted edits, normal F10 menu and input release are exercised.

Without that manifest, `-SettingsTestDll` selects an instrumented companion build
so Settings values remain private; installed assets accompany that DLL. Such a
run does not establish production Settings acceptance.
The runner records hashes, checks the selected browser and its loaded payload,
and refuses an existing Starfield session. It closes its own game normally, with
the harness's identity-checked cleanup as a fallback. No player profile or save
is edited. Failed runs and isolated fixture profiles are retained for diagnosis.
When using an isolated `-HarnessRoot`, the runner also reserves
`-SharedHarnessRoot` (the primary local harness by default) so concurrent test
tasks share its desktop lock. A held lock blocks launch without disturbing its
owner.

Checks cover automatic backend selection despite the obsolete opposite setting,
actual composited pixels, native replies/rejections, blocked networking, keyboard
text/Backspace/Tab/Space, mouse clicks/wheel, native select popups, relative dragging,
focus loss/return, events, retained state across document reload, real Papyrus VM
requests before and after load, legacy ABI 1.8 and web requests/state/events,
repeated menu opens, HUD movement, engine pause ownership, save/load,
owned browser-host termination/recovery, and a bounded animation/bridge soak
(`-SoakSeconds`, default 120). Game screenshots must contain two independent page
markers and should also be visually inspected. Native handlers returning are not
treated as completion of save/load or browser presentation.

Results are under `build/backend-validation/game`. Keep the failed artifacts when
fixing a defect, then run a fresh session. The standalone helper lifecycle and
performance tools live under `tools/profiling`; run those while Starfield is closed.

## Original consumer review

`-RealMods -SettingsManifest <manifest>` stages the installed Stock Exchange,
Starcade, AISS and Somatic Camera payloads byte for byte into the private mod,
enables their ESMs only in the private profile, and also isolates Starcade's
Documents state and the legacy `Data/SFSE/Plugins/OSFUI/settings/values` directory.
Legacy values are seeded from copies of the installed payload and MO2 overwrite;
the private VFS mapping also redirects new files and atomic replacements. Shared
overwrite hashes must remain unchanged. It never appends observers to consumer HTML or rebuilds their
DLLs/scripts. This mode waits up to 30 minutes for explicit review (override with
`-ReviewSeconds`, bounded to two hours); it does not
run the synthetic browser suite or infer success from the wait.
The enabled originals include `ImprovedCameraSF.esm`, which supplies Somatic's
invisible headgear. A copied DLL with that ESM disabled is not a valid camera fixture.
Inspect and dismiss original-mod startup prompts before manually checking the
production Settings menu and launcher handoff. The ordinary packaged suite checks
Settings on F10 automatically; original-mod review must record its own menu checks.

Once `review-ready.json` exists, an authorized operator can dot-source
`Review-Context.ps1 -RunPath <run> -Interactive` in another PowerShell session.
That provides the harness's PID/creation-checked input, console and capture
functions plus public API probes. Inspect original UI captures and actual game
state before recording each result. The final `review-complete.json` needs the
matching `session`, `outcome: "passed"`, a nonempty `checked` array, a `limitations`
array and `captures` records (`file`, `sha256`, nonempty `observation`). Failed
checks must use a failed outcome. A command receipt alone is not acceptance.

Cover Settings launcher handoff, edit/rebind callbacks and persistence, F7 Stock
Exchange data/orders/charts/reopens, F8 Companion Log navigation, F9 Starcade
gameplay/persistence and Somatic Camera settings/behavior. Keep missing original
packages and untested workflows explicit. `-ResumeRun <completed-run>` carries
private Settings/Starcade/legacy-provider values and the saved game into a fresh private run for
edition-switch checks; the old run remains intact. Resume waits for a new native
game-load completion before accepting the loaded state. A failed diagnostic run
cannot serve as an accepted switch baseline.

`-SomaticModRoot <extracted-original-Data-folder>` selects a complete, unchanged
downloaded Somatic payload instead of the installed copy, whose INI may contain
personal settings. Record the original archive identity with the review. Every
source file is hashed and checked after copying, and edition switching requires
identical original consumer payloads. This is a fixture choice, not the diagnostic
INI override. Settings edits made through the public API remain private and are
carried forward normally. Document diagnosed consumer limitations in the review;
do not describe an affected consumer workflow as passed merely because framework
delivery works.

`observeHotkey` with `mod`/`key` subscribes non-consuming counters through both
the public Settings provider and the legacy bridge. Compare `providerHotkeys`
and `legacyHotkeys` with the original mod's visible response to locate failures;
the counters themselves do not establish that the mod opened.

The private profile enables Papyrus tracing. The separate test script's
`OSFUIBackendPapyrus.ObserveInventory` records actual player credits for comparison
with Stock Exchange or Starcade transactions; console dispatch alone is not proof
that the observation ran.

For focused diagnosis, `-DiagnosticWithoutUI` omits OSFUI.dll from the private
process, `-DiagnosticSomaticIni <file>` uses an explicitly recorded camera INI,
and `-DiagnosticDisableQuickSaveLoad` disables that one native input handler at
startup in the owned process. These switches require a fresh `-RealMods` run,
never write a ControlMap file, and deliberately cannot pass release acceptance
or become a `-ResumeRun` baseline. They leave the original installed mods intact.
The separate `osfui-backend-test/aiss-layout` view measures computed styles using
an unchanged copy of AISS's stylesheet; it does not alter the original view.

`-Graphics luma` requires a locally installed Luma fixture. Stock results do not
establish Luma/HDR/frame-generation compatibility, physical controller or IME
behavior, every resolution/adapter combination, or operation on another machine
without Evergreen. Those remain separate acceptance checks.

## Background CEF runtime checks

`-RuntimeChecks` runs a focused CEF suite without desktop input or focus changes.
It requires a production Settings archive and an explicitly generated video:

```powershell
pwsh tests/game/Build-VideoFixture.ps1 -FFmpeg <path-to-ffmpeg.exe>
pwsh tests/game/Build.ps1
pwsh tools/package.ps1 -Backend cef -Tag game-runtime -OutDir build/cef-game/packages
pwsh tests/game/Test-Backend.ps1 -Backend cef -RuntimeChecks `
  -Package build/cef-game/packages/OSF-UI-v2.1.0-game-runtime-cef.zip `
  -SettingsManifest <production-settings-manifest.json> `
  -VideoFixture build/cef-game/fixtures/seek.webm
```

The consumer reads Starfield's D3D12 adapter LUID and compares it with the owned
Chromium GPU process. The page reports its hardware ANGLE renderer. The 20-second
1280x720 VP8 fixture has a moving background and timestamp-specific color patches;
the page checks Range responses and decoded canvas pixels after forward, backward
and near-end seeks, followed by resumed playback. Browser events or `currentTime`
alone cannot pass this check.

Two passive HUDs exercise a slow parser, failed navigation and automatic retry,
missing reveal, and a renderer crash. The runner identifies the sibling's
renderer by its controlled parser CPU loop among the owned helper's children,
then rechecks its PID/creation time before terminating only that process. This
also handles Chromium's spare renderers. Captures decode the healthy HUD's binary frame marker in the
actual game window, requiring distinct rendered frames across each scenario.

After a committed IndexedDB/localStorage/cookie write, the runner closes the game
normally and requires the helper to exit with code zero. A fresh game process
reads the same data. All browser data is private: with the harness lock held and
all Starfield/CEF processes stopped, the runner temporarily renames the original
`%LOCALAPPDATA%/OSFUI/CEF` directory, uses a marked empty test directory, retains
it with the run's evidence, then restores and hash-checks the original directory.
If processes remain, it preserves both directories and reports blocked cleanup.

This scoped suite does not claim keyboard/controller acceptance, every codec,
an additional game GPU, or Luma/HDR/frame-generation coverage. Failed attempts
remain in the artifact directory alongside successful runs.

### Verified October 19, 2026

Run `build/backend-validation/game/20261019-073300-298-cef-stock-runtime` passed
**68 assertions, zero failures**, with 16 game-window captures and two fresh
Starfield 1.16.244.0 processes. Test sources were commit `1248ec7`; the unchanged
production CEF package was built from clean `3c2b849`:

- Archive SHA-256: `53EFB38E92F97A9DA2EE3049C58E4ADA40621D326D16D6C4F05FEC81B6723C74`.
- Helper DLL SHA-256: `8AA5345C61B82B1E53E39B63E7DFB3FC1E328DDBB140D71A6D698E1099051F8C`.
- Production Settings 1.1.2 archive SHA-256: `EF5183DCBD6A72D7871780FF10EA1FA09621054913AE31F8A7F8B2137788E44A`.

| Check | Observed result |
| --- | --- |
| GPU selection | Native game D3D12 LUID `0,954964989` matched Chromium's GPU process; WebGL reported NVIDIA GeForce RTX 3080 / ANGLE D3D11. Game captures contained the rendered HUD. |
| Video seeking | 10,036,850-byte, 20-second VP8/WebM; bounded and suffix HTTP 206 responses; decoded colors matched seeks to 14.25, 2.5, 9.25 and 18.25 seconds, all at readyState 4; playback resumed. |
| View isolation | Distinct healthy-HUD game pixels during slow load, renderer crash, automatic recovery, failed navigation/retry and missed reveal. The healthy document survived the sibling renderer crash. |
| Profile persistence | 622,625-character payload survived in localStorage and IndexedDB, plus a persistent cookie. Normal game/helper shutdown after the write took 924 ms in total, helper exit code 0; a fresh game read the data. Both helpers exited cleanly. |

The original CEF profile was restored byte-for-byte and live Settings hashes
were unchanged. Consumer publication was error-free in both processes. No new
production CEF defect was found. The test consumer did expose a transient
Windows atomic-replace error; `1248ec7` adds bounded retries for access-denied
and sharing-violation errors without replaying mutations. Earlier diagnostic
attempts are retained; the final run above supersedes their probe failures.

This is stock-graphics acceptance on this machine's RTX 3080 at a 3440x1409 game
client size. Other game adapters, codecs, Luma/HDR/frame generation and interactive
input remain outside this scoped run. The installed WebView2 edition was retained.

### Final 2.1 archives — October 19, 2026

Both final archives from clean `b8b701f` were subsequently tested with the exact
production Settings 1.1.2 artifact above:

- `20261019-212157-179-webview2-stock`: **100 assertions passed, zero failures**,
  114 completed observations, 10 visually reviewed captures. Includes keyboard,
  mouse, focus, Settings F10, menu/HUD, pause, legacy/native/Papyrus, save/load,
  helper recovery and the full 120-second animation/bridge soak.
- `20261019-212526-736-cef-stock-runtime`: **68 assertions passed, zero failures**,
  56 completed observations and 16 captures across two fresh games. Repeated GPU
  matching, decoded video seeking, view isolation, storage flushing/restoration
  and graceful exit against the final CEF ZIP.

Both runs used stock graphics on RTX 3080 with a 2560x1409 client. Live Settings
storage was unchanged, helpers exited with code zero, and there were no evidence
warnings or device/hook failures. The original CEF profile was restored exactly.
No new runtime defect was found. The [release record](../../packaging/release-2.1.md)
pins both archive hashes, source, dependency, deployment and acceptance limits.
