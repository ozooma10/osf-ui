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
`dist`. `-SettingsTestDll` selects the companion's instrumented build so Settings
values remain private. The current installed Settings assets accompany that DLL.
The runner records hashes, checks the selected browser and its loaded payload,
and refuses an existing Starfield session. It closes its own game normally, with
the harness's identity-checked cleanup as a fallback. No player profile or save
is edited. Failed runs and isolated fixture profiles are retained for diagnosis.

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

`-Graphics luma` requires a locally installed Luma fixture. Stock results do not
establish Luma/HDR/frame-generation compatibility, physical controller or IME
behavior, every resolution/adapter combination, or operation on another machine
without Evergreen. Those remain separate acceptance checks.
