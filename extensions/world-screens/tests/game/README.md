# Addon integration checks

`Start-Addons.ps1` stages the matching core, extension, Starcade world payload and
SSSE display payload into a new private MO2 mod/profile. Build those payloads first
using their repository helpers. It uses the existing OSF Test Harness and its pinned
disposable save, runner lock, owned-process handshake and background console mailbox.
It redirects saves, Settings values and Starcade progress to run-specific folders.
It never enables content in the source profile or publishes anything.

```powershell
pwsh extensions/world-screens/tests/game/Start-Addons.ps1
# Explicitly authorized keyboard/mouse checks:
pwsh extensions/world-screens/tests/game/Start-Addons.ps1 -Backend webview2 -Interactive
pwsh extensions/world-screens/tests/game/Start-Addons.ps1 -Backend cef -Interactive `
  -CoreData 'C:/Modding/Starfield/OSF UI/build/world-addon-migration/editions/cef/Data'
```

`Backend` defaults to `webview2`. `CoreData` defaults to
`build/package/staging/<backend>/Data`; the package checker rejects a mismatched
edition before launch. Each run stages both private Papyrus probes before MO2/VFS
starts. `Interactive` must be supplied for harness keyboard/mouse/focus commands.

The run directory is recorded in `build/world-addon-migration/active.json`. To use
the existing harness commands against that owned process:

```powershell
$active = Get-Content build/world-addon-migration/active.json -Raw | ConvertFrom-Json
. extensions/world-screens/tests/game/Context.ps1 -RunPath $active.run.path
# Acquire the shared harness state/runner.lock before command sequences.
Invoke-ConsoleCommand 'coc StationTheKeyInterior'
Read-Consumer
Save-Capture 'key-monitors'
Stop-TestGame
```

Use `$runPath` for a path variable: PowerShell is case-insensitive, and the harness
owns `$script:Run` as its artifact dictionary. Do not assign a path to `$run`.

Use `0x` prefixes for hexadecimal console form IDs. Command receipts are not
completion evidence: wait for actual documents, rows, bindings or save generations.
Consumer `pages` retain their last report; check document IDs and reported tick
timestamps when testing reload. Project filesystem observations to paths/sizes,
not raw FileInfo objects, before passing them to `Wait-Observed`. Wrap collections
in a property (for example `@{files=@(...)}`) to avoid single-item unwrapping.
`Read-WorldPage` returns only reports newer than three seconds.

The private Papyrus probe can start/release the real Red Mile game through Starcade's
native API and instantiate the actual craftable display bases. `observe.js` reports
the real page DOM, prices and game iframe. It loads the web helper only when absent,
so observation cannot replace an addon's existing state/event subscriptions.
Neither probe nor observer belongs in production packages.
The observer also records key down/up events delivered to the real game iframe and
samples Orbital Paddle's canvas; these observations do not simulate a controller.
`EndRedMile` deliberately releases the reference binding. It verifies interaction
teardown, not normal page/Escape exit or restoration of the idle poster. Do not
immediately refresh that binding before its asynchronous terminal callback.

Default runs use native console commands and background window capture. Interactive
runs use the harness's bounded keyboard/mouse helper, owned PID/creation identity
and shared runner lock. Keep console setup in the mailbox, never typed into the UI.
Normal shutdown and `Finish-RunArtifacts` retain the evidence.

For ship Decorate checks, copy a suitable ship save into the private save directory
and retain its source hash; never save over the source. The pinned QASmoke fixture
does not necessarily have a player ship. `OSFWorldWorkshopProbe.Supply` supplies the
inventory MISC forms (Zero Wire `0x246B65`, Beryllium `0x57D9`, Aluminum `0x557D`);
recipe component IDs are not usable inventory items. Enter scanner with F, then R
for Decorate. Build both objects from Decorations, use Tab for Modify, E to move,
and hold R to delete. Observe real resource consumption, rendering and removal.
Avoid curved/recessed walls when judging the full screen face. Fresh processes may
remap dynamic reference IDs; resolve them again rather than reusing an earlier FF ID.

Activate Starcade with E and choose Play; inspect the highlighted menu item before
confirming. Check ArrowUp/Enter, Escape, returned player controls, focus loss and
re-entry. Configure SSSE through its real company menus, dismiss the quote dialog,
then save/load and require a new document ID with fresh selected-company data.
Physical controller, all-game and outpost coverage require separate acceptance.
The historical Starcade `tests/cabinet` prototype is not this integration runner.
See [the recorded addon validation](../../ADDON_VALIDATION.md) for the successful
2026-10-19 run and its exact evidence.
