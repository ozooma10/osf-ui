# OSF UI 1.6.0 release smoke — 2026-09-18

**Passed for RC4.** Four fresh game sessions completed with the production
package, two with stock rendering and two with Luma. The startup regressions
found in RC2/RC3 were corrected and rerun with both renderers. Use RC4 for the
hotfix; earlier candidates are superseded. Nothing was committed or published.

## Candidate and environment

- Branch: `hotfix-1.6.0`, based on release `v1.5.0`.
- [Candidate ZIP](../dist/OSF-UI-v1.6.0-rc4.zip): 30 files, 8,534,057 bytes.
- SHA-256: `d287fa8927cf599b80b6d3ba4c81a324e58c237cd4904c24d700e73d211b4a06`.
- The ZIP hash and all 30 extracted file hashes were verified before launch.
- Starfield 1.16.244.0, RTX 3080, WebView2 153.0.4234.32, installed Luma 2.0.0
  with its Shader Injector dependency.
- Private `OSF Testing` profile and restored disposable save. Settings Slim
  and Character Studio were disabled in every test, including their test builds;
  loaded-module evidence confirms their DLLs were absent.
- Production `devMode=false`. A separate test mod preloaded a small 1.5-protocol
  view and schema. Its only writes were to `osfui.hotfix.json` in private test output.

## Completed matrix

| Fresh session | Startup menu | Assertions | Completed observations | WGC captures | Result |
| --- | --- | ---: | ---: | ---: | --- |
| [Stock](<C:/Modding/Starfield/OSF Test Harness/artifacts/20260918-152513-572-OSFUI-Stock/report.md>) | Off | 17 | 41 | 7 | Passed |
| [Luma](<C:/Modding/Starfield/OSF Test Harness/artifacts/20260918-152607-039-OSFUI-Luma/report.md>) | On | 21 | 40 | 7 | Passed |
| [Luma restart](<C:/Modding/Starfield/OSF Test Harness/artifacts/20260918-152659-685-OSFUI-LumaRestart/report.md>) | Off | 19 | 41 | 7 | Passed |
| [Stock restart](<C:/Modding/Starfield/OSF Test Harness/artifacts/20260918-152750-944-OSFUI-StockRestart/report.md>) | On | 20 | 40 | 7 | Passed |

Totals: **77 assertions, 162 completed observations, 28 completed captures**.
Each session took approximately 51–53 seconds including startup. No artifact
collection warnings were reported. Run folders contain session identities,
native observations, input results, profile snapshots, binary hashes, and logs.
The [batch evidence](<C:/Modding/Starfield/OSF Test Harness/artifacts/20260918-152510-OSFUIRelease/results.json>) ties all four runs to the same archive.

## What passed

- Abandoned process mirrors were removed at startup. A live controller-process
  mirror and an unrelated marked directory survived.
- A deliberately locked abandoned mirror survived the first launch and was
  removed after unlocking on the next launch. Each subsequent run removed the
  prior actual game session's mirror.
- Built-in Mods and a third-party view rendered. Native focus-menu observations,
  completed screenshots, and visual inspection established actual UI behavior.
- Real keyboard K and mouse clicks reached the browser document. The document
  reported receipt through the production settings bridge and displayed the result.
- Bridge handshake reported OSF UI 1.6.0 / protocol 1.5. Ping, settings write,
  readback, disk persistence, and next-session counter increments passed.
- Each run completed eight additional open/close cycles, for 32 repeat cycles,
  as well as the initial built-in and third-party menu visits.
- The game lost foreground to a temporary harness-owned window and regained it.
  The overlay remained rendered and usable afterward.
- Escape closed the third-party view; vanilla PauseMenu opened and closed;
  bounded forward input then changed the native player position. Input and
  simulation were released after the overlay closed.
- Startup menus were applied without a crash and closed safely during the
  MainMenu/LoadingMenu transition, as the released runtime contract specifies.
- Both Luma runs recognized/chained `Luma.dll` and produced a first overlay draw
  into `R16G16B16A16_FLOAT`. Both stock runs used `R8G8B8A8_UNORM`.
- No hook failure, OSF UI error, device-loss signature, or game crash occurred
  in the final matrix. The intentional file-lock warning and transient focus
  watchdog recovery warnings were retained and checked against test actions.

Representative inspected captures:
[Luma built-in menu](<C:/Modding/Starfield/OSF Test Harness/artifacts/20260918-152607-039-OSFUI-Luma/builtin-mods-menu.png>),
[Luma keyboard/mouse result](<C:/Modding/Starfield/OSF Test Harness/artifacts/20260918-152607-039-OSFUI-Luma/third-party-input-confirmed.png>),
[Luma focus return](<C:/Modding/Starfield/OSF Test Harness/artifacts/20260918-152607-039-OSFUI-Luma/after-focus-return.png>),
[stock vanilla pause](<C:/Modding/Starfield/OSF Test Harness/artifacts/20260918-152750-944-OSFUI-StockRestart/vanilla-pause-after-overlay.png>).

## Failures found and corrected

RC2 crashed with a startup-configured menu before game data loaded. The retained
[crash diagnosis and log](<C:/Modding/Starfield/OSF Test Harness/artifacts/20260918-151800-202-OSFUI-Luma/diagnosis.md>) identify
`ControlLayer::Apply` through `Runtime::Tick`: changing input-enable masks called
player-control listeners before their initialization was complete. Engine focus,
control, simulation-pause, and cursor effects now wait for post-data-load UI setup
to publish readiness. Effects still execute on the existing queued runtime tick.

RC3 avoided that crash but failed the startup-policy assertion: an empty menu
request queue skipped policy application after deferred hook installation.
RC4 explicitly reapplies the queued startup policy once on the first tick after
installation completes. [RC3 evidence](<C:/Modding/Starfield/OSF Test Harness/artifacts/20260918-152306-396-OSFUI-Luma/report.md>) is retained.

The first harness attempt also encountered a shared-log read mode issue; the
reader now permits the game's active writer. The minimize-based focus probe
could not minimize a game with its parented WebView active, so the new
`focus-transfer` probe uses its own temporary window and verifies real loss
and restoration of foreground. These were harness corrections, not packaged
runtime changes. Earlier blocked runs remain in the artifact directory.

## Cleanup and limits

The private profile's mod list was restored byte-for-byte. All four preexisting
user settings files retained their original hashes. The test game, WebView
helper, and MO2 launcher were stopped. Marked cache probes were removed.
Only the latest production process mirror (`views-mirror-32516`) remained;
it is intentionally eligible for cleanup at the next launch. Development,
research/world mirrors, helper binaries, and browser-data directories remain
outside this hotfix's scavenger scope.

The native SDK, Papyrus payload, and private helper protocol header still match
the 1.5.0 release. Production compilation, packaging checks, focused native
regression tests, and `git diff --check` passed.

This establishes automated desktop smoke acceptance on this machine. It does
not measure physical HDR color/brightness, exercise a physical gamepad, verify
all resolutions/mod combinations, or establish long-session endurance. Luma's
floating-point UI path is directly verified; HDR panel calibration is separate.

Re-run instructions and fixture details are in
[the harness scenario guide](<C:/Modding/Starfield/OSF Test Harness/scenarios/OSFUIRelease.md>).
