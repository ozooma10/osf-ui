# World-surface runtime test

This private scenario reuses the OSF Test Harness's owned sessions, disposable
save, native observations, real Windows input, WGC captures, and retained reports.
It does not modify the harness source. Run with PowerShell 7 from the OSF UI root.
See [recorded validation](VALIDATION.md) for the successful runs and defects found.

The fixture places a vanilla DisplayScreen5 in QASmoke, the cell used by the
current pinned harness save. It temporarily overrides the known-good Lodge
color/emissive texture paths **only in the private test mod**. These assets must
not be included in a production package. See [fixture provenance](fixtures/README.md).

First establish that the fixture renders without the OSF UI DLL:

```powershell
pwsh -NoProfile -File tests/world-surface/Invoke-WorldSurfaceTest.ps1 -Mode Baseline -AimAtScreen -KeepGame
pwsh -NoProfile -File tests/world-surface/Invoke-WorldSurfaceTest.ps1 -Action Capture -CaptureName baseline-inspection
pwsh -NoProfile -File tests/world-surface/Invoke-WorldSurfaceTest.ps1 -Action Stop
```

Inspect `baseline-screen.png`: the screen should show the checkerboard and the
surrounding world should remain visible. `-AimAtScreen` uses bounded console
input to place the disposable player at `(-4.8, 4.2)` south of the screen and
angles `(0, 0)`, then pitches the saved third-person camera downward through
one relative mouse move. Native position readback verifies placement. This is
specific to the current pinned fixture's starting camera; inspect the PNG to
verify framing. Repeated captures in that retained session do not aim again.

Build the current runtime with its test snapshot logging, then run the live test:

```powershell
xmake f -P . -m releasedbg --test_harness=y -y
xmake build -P . -j1 "OSF UI"
pwsh -NoProfile -File tests/world-surface/Invoke-WorldSurfaceTest.ps1 -Mode Live -AimAtScreen -KeepGame
pwsh -NoProfile -File tests/world-surface/Invoke-WorldSurfaceTest.ps1 -Action Status
pwsh -NoProfile -File tests/world-surface/Invoke-WorldSurfaceTest.ps1 -Action Stop
```

For the combined host lifecycle check, add both optional flags on a fresh run:

```powershell
pwsh -NoProfile -File tests/world-surface/Invoke-WorldSurfaceTest.ps1 -Mode Live -AimAtScreen -Overlay -RestartBrowser -KeepGame
```

`-Overlay` stages a separate passive HUD with a small top-right `OVERLAY HOST`
counter. Its startup flag is off. The compile-gated test observation hook requests
it once through the public native Views API only after the exact world fixture
has completed GPU copies, the player is in QASmoke, and MainMenu/LoadingMenu are
closed. The overlay must exist in the discovered view catalog. This keeps the
coexistence check in loaded gameplay and leaves world-only runs untouched. The runner waits
for that request, two independent helpers belonging to this game, and the
ordinary overlay draw marker while world frame copies continue.
`-RestartBrowser` then stops only the world helper after checking its exact
executable path/hash, game PID, `world-0` instance, and creation time. The test
rechecks PID/start identities before stopping it, waits for a replacement process
and a newer shared-ring generation with completed fresh frames, and saves a WGC
capture. With both flags, the original overlay helper must survive unchanged.
This deliberately exercises browser-loss recovery; it does not restart the game.
Both test pages send the native `osfui.hello` handshake and show `BRIDGE READY`
only after receiving a `ready` response. Check that label again on the physical
screen after browser recovery, in addition to its advancing frame counter.

Live mode copies the deployed DLL/helper and payload into `OSF Testing - World
Surface`, stages an opaque animated `kind: world` view, and enables the screen
plugin only for this run. The normal `OSF Settings` deployment remains enabled
as OSF UI's dependency; set `-SettingsModName` if its installed folder differs.
`-RuntimeMod` can select a different already-built OSF UI payload.

`Run` launches, checks observations, captures evidence, and stops its game unless
`-KeepGame` is supplied. `Start` launches for inspection without requiring render
assertions. `Status`, `Capture`, and `Stop` operate only on the retained session.
Every fresh run starts from the standard pinned disposable save. Existing games
block the test. The runner restores the exact private profile files after stopping;
it never modifies the source profile. Use `Stop` to recover a retained session.

Structured assertions require bound material descriptors, completed GPU copies
of advancing browser frames, stable descriptor count, and resumed copies after
a controlled focus loss, without device or shared-ring failures.
The world view must not open the overlay focus menu. The run records all fixture
and runtime hashes, exact staged plugin/mod lists, logs, process identity, native
observations, and completed WGC PNGs under the harness's `artifacts` directory.
Inspect the PNGs to establish that the changing content is on the physical screen,
that it has the expected orientation, and that the surrounding world renders.
Counters and successful capture do not establish those visual properties.

The vanilla DisplayScreen5 donor maps approximately the top half of its texture
onto the screen face. The test card therefore keeps all text and animation in
the top 48% of the 1000x1000 browser image. This is a fixture UV constraint, not
cropping performed by the texture transport. A release screen mesh/material
should define its own predictable UV mapping.

After testing, restore the normal build configuration and deployment:

```powershell
xmake f -P . --test_harness=n -y
xmake build -P . -j1 "OSF UI"
```
