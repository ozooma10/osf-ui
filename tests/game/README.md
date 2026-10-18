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
Documents state. It never appends observers to consumer HTML or rebuilds their
DLLs/scripts. This mode waits up to 30 minutes for explicit review; it does not
run the synthetic browser suite or infer success from the wait.

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
private Settings/Starcade values and the saved game into a fresh private run for
edition-switch checks; the old run remains intact.

`-Graphics luma` requires a locally installed Luma fixture. Stock results do not
establish Luma/HDR/frame-generation compatibility, physical controller or IME
behavior, every resolution/adapter combination, or operation on another machine
without Evergreen. Those remain separate acceptance checks.
