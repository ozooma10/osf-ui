# World-surface runtime test

This private scenario reuses the OSF Test Harness's owned sessions, disposable
save, native observations, real Windows input, WGC captures, and retained reports.
It does not modify the harness source. Run with PowerShell 7 from the OSF UI root.
See [recorded validation](VALIDATION.md) for the successful runs and defects found.

## Multiple independent displays

`-Screens 2`, `3`, or `4` generates a grid of vanilla display meshes with separate
private materials, byte-identical black 64x64 placeholders, and stock-board views
from two mod namespaces. Materials use generated `textures/osfui/feeds/...`
bindings; palette assets remain under `textures/OSFUIWorldTest`. No mode overrides
Lodge textures. The names/prices are fictional. The test DLL sends a
different `stock.update` event to each view through the public Views service.

```powershell
pwsh -NoProfile -File tests/world-surface/Invoke-WorldSurfaceTest.ps1 -Mode Baseline -Screens 4 -AimAtScreen -TestModName 'OSF Testing - World Boards'
pwsh -NoProfile -File tests/world-surface/Invoke-WorldSurfaceTest.ps1 -Mode Live -Screens 4 -AimAtScreen -RestartBrowser -Overlay -TestModName 'OSF Testing - World Boards' -RuntimeMod '<worktree>/build/isolated-mods/OSF UI'
```

Build worktree payloads with process-local `XSE_SF_MODS_PATH` pointing to
`<worktree>/build/isolated-mods` and `XSE_SF_GAME_PATH` unset. Pin XMake to that
worktree (`-P .` from its root), configure `--test_harness=y`, and build as above.
Set `WEBVIEW2_SDK_DIR` to an unpacked SDK if the worktree lacks `external/webview2`.
Use a distinct `-TestModName` for a worktree: the runner refuses to overwrite a
test mod owned by another checkout. No main-checkout source or ordinary deployment
is changed. Retained sessions remember their screen count and test-mod identity.

The multi-screen assertions require every material to bind, every GPU stream to
advance with stable descriptors, and one verified helper per display. Restart
checks require the other helpers to keep their process identities, ring
generations, and advancing frames; the optional overlay must survive too. Inspect
the captures for four different symbols/prices and their changing update counts:
GPU counters alone cannot prove that the correct page is visible on each mesh.
Baseline captures should show four black screen faces and a normal world.
Multi-screen camera framing uses the pinned third-person view from `(-5.7, 1.0)`
so the player does not obscure the boards. Single-screen framing stays unchanged.

## Single-screen fixture

The fixture places a vanilla DisplayScreen5 in QASmoke, the cell used by the
current pinned harness save, with a private material and generated texture asset.
The preserved plugin is rewritten by `world_boards.py` even for one screen.
See [original fixture provenance](fixtures/README.md).

First establish that the fixture renders without the OSF UI DLL:

```powershell
pwsh -NoProfile -File tests/world-surface/Invoke-WorldSurfaceTest.ps1 -Mode Baseline -AimAtScreen -KeepGame
pwsh -NoProfile -File tests/world-surface/Invoke-WorldSurfaceTest.ps1 -Action Capture -CaptureName baseline-inspection
pwsh -NoProfile -File tests/world-surface/Invoke-WorldSurfaceTest.ps1 -Action Stop
```

Inspect `baseline-screen.png`: the screen face should be black and the
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

## Exact asset identity and lifetime

The fixture's four feeds are `osfui-world-test/screen`,
`osfui-world-test/screen-2`, `z-world-test/screen`, and `z-world-test/screen-2`.
All placeholders have identical format, dimensions, and pixels; only their
SHA-256-derived asset paths differ. All browser outputs are 1000x1000.

After a successful four-screen `Run -KeepGame`, execute:

```powershell
pwsh -NoProfile -File tests/world-surface/Invoke-TextureLifetimeProbe.ps1 -Action Cycle -Cycles 3
pwsh -NoProfile -File tests/world-surface/Invoke-WorldSurfaceTest.ps1 -Action Stop
```

Each cycle requests eviction while all materials still own their resources,
asserts the outputs remain resident, leaves QASmoke and purges its cell buffers,
waits for zero output residency, returns, and requires new output generations
with the same exact feed IDs. Engine leases, copy/initialization fences and the
post-ownership DIRECT fence must all retire before any allocation is released.
`texture-lifetime-cycles.json` preserves the before/held/empty/reloaded snapshots.
Console commands use only the owned disposable game and refocus before each
operation. The runner intentionally leaves an interrupted session owned for
inspection and `-Action Stop`; never stop an unrelated game.

Run allocation pressure on a fresh game:

```powershell
pwsh -NoProfile -File tests/world-surface/Invoke-WorldSurfaceTest.ps1 -Mode Live -Screens 4 -AimAtScreen -BudgetPressure -TestModName 'OSF Testing - World Boards' -RuntimeMod '<worktree>/build/isolated-mods/OSF UI'
```

The compile-gated fixture lowers the output budget to 8 MiB. It requires two
admitted feeds and two explicit allocation deferrals, and verifies deferred
feeds own no replacement descriptors. Inspect the PNG: two correct feeds and
two black faces, with no other feed substituted. Shipping builds ignore this
private budget-control file. The eviction request file is staged before launch
so MO2's virtual filesystem can see later updates.

This is the texture integration milestone. The fixture still uses dedicated
browsers and continuous updates; it does not prove shared snapshot workers,
idle browser release, job/session isolation, or the future cached-feed API.
