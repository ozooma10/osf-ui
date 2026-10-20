# Private WebView2 world-screen audio runs

These helpers operate the owned game through the existing native test mailbox.
They share the test harness `runner.lock`, use a fresh MO2 mod/profile and save
directory, and enable browsing/developer diagnostics only in the private Settings
mapping. The normal browser profile is moved aside under a session-specific lease
and restored after every game/browser host has exited. Audio measurements and
lifecycle commands run in the background. Watch admission requires the game to
be foreground; pass `-Interactive` only after explicit user authorization.

## Prerequisites and launch

Use PowerShell 7 with the configured OSF Test Harness, its native DLL/input helper,
an existing fixture save, the backend test consumer, and Papyrus compiler/imports.
Stage the intended WebView2 core payload under
`build/webview2-spatial-game/core`, or pass another prepared data root using
`-CoreData`. The normal MO2 `OSF UI - World Screens` and `OSF Browser TV` payloads
must match the implementation under test. Start with all Starfield and WebView2
host processes stopped.

From the repository root:

```powershell
pwsh -NoProfile -File tools/profiling/audio-endpoint-meter/build.ps1
pwsh -NoProfile -File tools/profiling/webview2-game-audio/Start.ps1
$run = (Get-Content -Raw build/webview2-spatial-game/active.json | ConvertFrom-Json).run.path
```

`Start.ps1` resolves the repository from its own location. Optional
`-RepositoryRoot`, `-HarnessRoot`, `-CoreData`, and `-PapyrusSource` parameters allow
other checkouts/layouts; harness and imports default to sibling directories. It
copies the trusted chrome observer and quiet tone fixture into the private mod.
The source supplied to ordinary web pages receives no test bridge or script.

`context.json` identifies the run, private paths, session, and harness. Preserve
it together with `browser-profile-lease.json`; recovery uses those identities.
Only one helper should operate the shared harness at a time. Do not bypass its
lock or start a second run while an earlier browser profile lease is unresolved.

## Control and measurement

```powershell
pwsh -NoProfile -File tools/profiling/webview2-game-audio/Control.ps1 -RunPath $run -Action Setup
pwsh -NoProfile -File tools/profiling/webview2-game-audio/Scenario.ps1 -RunPath $run -Interactive
python tools/profiling/webview2-game-audio/analyze.py $run
```

For individual phases (use a fresh evidence name if the scenario already ran):

```powershell
pwsh -NoProfile -File tools/profiling/webview2-game-audio/Control.ps1 -RunPath $run -Action Meter -Name one-tv
pwsh -NoProfile -File tools/profiling/webview2-game-audio/Control.ps1 -RunPath $run -Action SpawnTwo
pwsh -NoProfile -File tools/profiling/webview2-game-audio/Control.ps1 -RunPath $run -Action Snapshot -Name two-tvs
pwsh -NoProfile -File tools/profiling/webview2-game-audio/Control.ps1 -RunPath $run -Action Watch -Interactive
pwsh -NoProfile -File tools/profiling/webview2-game-audio/Control.ps1 -RunPath $run -Action Browser -Value close
pwsh -NoProfile -File tools/profiling/webview2-game-audio/Control.ps1 -RunPath $run -Action Browser -Value navigate -Payload '{"input":"https://media.w3.org/2010/05/sintel/trailer.mp4"}'
pwsh -NoProfile -File tools/profiling/webview2-game-audio/Control.ps1 -RunPath $run -Action Ready -Value 'https://media.w3.org/2010/05/sintel/trailer.mp4'
```

`Setup` requires the pinned fixture cell, spawns a real TV, starts the 997 Hz
trusted-chrome tone, and waits for capture plus a fresh scene. `SpawnTwo` creates
another real TV and records its form ID. Other actions are `Tone` (`start`/`stop`),
`Browser` (a browser command and JSON `-Payload`), `Console` (native mailbox command
in `-Value`), and `Capture` (background WGC). Evidence names accept lowercase
letters, numbers, and hyphens. Use a new name for each meter phase.

`Browser` confirms command acknowledgement. `Ready` separately waits for the exact
requested URL, completed loading, no landing/error state, and fresh content
capture before measuring a remote page.

Each meter directory contains a read-only `result.json`, binary/source provenance,
and before/after snapshots of page, audio-route, and game state. If the meter
fails, the helper still attempts an after snapshot. An unavailable page/audio
route is represented by null, which is useful evidence during teardown; it is
not automatically a successful silence test.

Use the actual scene camera's position and forward/up basis for panning
assertions. Requested player yaw can differ from the rendered camera, especially
in free-camera mode. Check emitter count/identity and current scene freshness
before measuring two-TV, movement, range, mute, or volume cases. Freeze listening
geometry for gain comparisons. After save/load or browser recreation, restart
the fixture tone and require the expected new document/binding identities.

`Lifecycle.ps1 -Action SaveLoad` verifies a unique private checkpoint and renewed
bindings; `-Action CellUnload` verifies the old TV host retires after cell transfer.
`CrashCollector.ps1` deliberately terminates exactly one validated extension
renderer in the owned profile, then checks recovery without recreating sources.
An active remote source must regain capture at the same URL; inactive `about:blank`
must remain blank and error-free while waiting for a real URL. The report records
which expectation applied. Use a new evidence name for each attempt; failed runs
remain intact.
Measure output separately before/after these lifecycle checks. If a scenario
fails midway, inspect its last completed phase before changing free-camera or
master-volume state; do not rerun a sequence blindly.

## Endpoint limits

The meter uses default-render-endpoint loopback, retaining numeric aggregates
only. It never changes mixer/session volume or saves desktop PCM. It includes
game ambience and other desktop sounds; this is not process-isolated capture.
Use `endpoint.toneRms`/`channelToneRms` for the controlled 997 Hz fixture and keep
the endpoint stable. Inspect `endpoint.format.channelMask` before interpreting
the first two channels as left/right; mask bits 0 and 1 must identify that pair.

For ordinary video, keep the default tone setting and use the always-available
broadband RMS with quiet-background evidence,
live route state, and source/page continuity. A changing soundtrack cannot prove
precise cross-phase gain, distance, restoration, or capture-only PCM amplitude.
Silence can produce zero endpoint packets; active tests need sufficient frames.
Reject meter errors, discontinuities, invalid samples, and endpoint/layout changes.
Native process exit zero means measurement completed, not scenario acceptance.

Watch/close, navigation, mute, save/load, cell detach, and multiple TVs need their
own contemporaneous observations. An API receipt or a healthy route flag alone
does not prove audible output. Standalone browser tests and game measurements
remain separate from listening quality, latency, and audio/video synchronization.
Run-specific pass/fail counts belong in that run's assessment, not this workflow.

## Normal exit and recovery

While the owned game is still running, finish through its native `qqq` command:

```powershell
pwsh -NoProfile -File tools/profiling/webview2-game-audio/Finish.ps1 -RunPath $run -Outcome passed -RestoreDefault
```

Choose `passed`, `failed`, or `blocked` from the actual scenario results. Finish
waits for normal game exit and WebView2 host shutdown, restores the leased browser
profile, compares canonical path/SHA-256 manifests, and collects run artifacts.
`-RestoreDefault` additionally returns the configured MO2 instance to Default;
omit it when continuing private tests.

If the game already exited and cleanup failed, do not relaunch it or call Setup.
Use the recovery helper, which does not require a live game:

```powershell
pwsh -NoProfile -File tools/profiling/webview2-game-audio/Restore.ps1 -RunPath $run
```

Restore validates the profile lease/marker, restores the original profile, and
records the run as failed. It leaves MO2 unchanged unless `-RestoreDefault` is
explicitly supplied. Live game/host processes, ownership/path mismatches, or hash
differences block restoration and retain evidence rather than overwrite a profile.
Keep the run's retained `browser-profile` directory for diagnosis; it contains the
private browsing profile and should not be published as a generic test artifact.
