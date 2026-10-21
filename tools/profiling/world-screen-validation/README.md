# World-screen game validation

These checks use the sibling OSF Test Harness, a private MO2
profile, a copied save fixture, private Settings values, and a leased TV browser
profile. They default to background operation and never inject keyboard/controller
input. Read the harness instructions before running them. Only one runner may own the game.

`Start.ps1` requires `-Backend webview2|cef`, `-CoreData`, and `-ExtensionData`.
Use matching core and extension builds, including the reserved TV view in the
extension payload. `-Label current|baseline` identifies performance captures.
The resulting context is recorded under `build/world-screen-validation/runs/`;
`active.json` points to the most recently prepared run. A failed or interrupted
run must be restored before another browser-profile lease is created.

Run `Cases.ps1 -RunPath <run> -Case <case> -Name <unique-name>` sequentially:

- `Setup`: create one real Browser TV and measure the controlled 997 Hz tone.
- `MultipleTVs`: add a second TV, verify the logical screen count, disable and
  reenable the TVs, and measure output before and after. Baseline builds with the
  known screen-count defect can use `SpawnTwo` instead.
- `SaveLoad`: write a unique private checkpoint, load it, observe renewed
  documents and bindings, restart the fixture tone, and measure output.
- `HostRecovery`: terminate only the pinned owned TV helper, observe its
  replacement, and measure recovered output.
- `StalledRecovery`: suspend only that helper using an identity-checked handle;
  normal production heartbeat recovery must retire it and replace the document.
  The handle is resumed and closed in `finally` if recovery fails. Native drain
  samples measure responsiveness, not rendered frames. Fault trials wait for
  65 healthy seconds after a preceding successful recovery so the production
  retry budget has reset.
- `Navigation`: stop an observed pending load, then observe a missing-domain
  failure. This does not establish postcommit transport-error coverage.
- `Video`: exercise the remote media route and mute/unmute. Endpoint broadband
  energy includes other desktop/game audio and does not prove gap-free playback.
- `CellUnload`, then `CellReload`: observe host retirement away from the TVs,
  return through the verified private checkpoint, and measure recovered audio.

Use `Finish.ps1 -RunPath <run> -Outcome passed|failed|blocked -RestoreDefault`
after a live run. It exits the owned game normally, preserves evidence, restores
the original browser files by hash, and cleans registered temporary MO2 folders.
If the game already exited, use `Restore.ps1` instead. Neither script can restore
browser files while another game/helper is running. `-RestoreDefault` restores
the starting Default MO2 profile used by this fixture.

## Performance

Stop builds and unrelated workloads before capture. Establish the same save,
camera, display settings, frame cap, focus state, and TV layout for both revisions.
Keep unrelated source changes identical. Use separate complete payloads and
retain their source revisions and hashes.

With explicit user authorization, `Focus.ps1 -RunPath <run> -AllowForeground`
focuses only the identity-verified owned game and records the observed result.
It sends no keyboard or mouse input. Otherwise the runner remains background-only.
Foreground matters here: a background game can enter the vanity camera orbit.

`Measure.ps1 -RunPath <run> -Variant current|baseline -Workload <name> -Repeat 1
-DurationSeconds 30` samples owned game/browser CPU and memory plus PresentMon
application-frame timing. Repeat each workload three times after warmup. CPU
100% means one logical core. Game CPU includes the whole game, and private memory
does not measure VRAM. The sampler verifies loaded DLL/helper hashes, rejects
foreign helpers, and records process identities and trusted-page observations.
Native samples require a fresh, unpaused game with stable focus, cell, position,
and camera pose (from the spatial listener). Private quality/display preferences
and actual captured window dimensions must match; stored preferred window sizes
are not treated as the actual rendering dimensions.
Comparisons reject mismatches between revisions. Use `-NoPresentMon` for CPU and
memory measurements when ETW privileges are unavailable; this supplies no FPS
or rendered-frame-pacing evidence.

For a recovery capture, add `-CaptureKind recovery -DurationSeconds 45`. Start
the sampler in a separate process, wait until its manifest says `recording`,
then trigger `StalledRecovery` after five seconds. Recovery comparisons permit
expected document/process changes and compare frame timing only; CPU during
process replacement is not a steady-state workload.

Run `python analyze-performance.py <baseline-run> <current-run> --out <new-dir>`.
The analyzer pairs repeat IDs, excludes mismatched or invalid steady captures,
and reports medians, ranges, and paired deltas. Results are descriptive, not a
significance test. Keep failed captures alongside accepted evidence.

`Run-Performance.ps1` runs a complete isolated comparison session from an explicit
payload: two TVs, 120-second warmup, three 30-second idle captures and three tone
captures, then normal exit and profile restoration. It requires the existing
explicit authorization to focus the owned game. Use different `-FirstRepeat`
values for independent launches and reverse variant order on the second pair;
`-ToneFirst` also permits matching the earlier tone-before-idle workload order.
Use `-ValidateControls` on a candidate session to include screen count, save/load,
navigation, video/mute and sustained audio checks after performance measurement.

The private observer counts browser-state events and DOM mutation records without
retaining DOM nodes. These counters distinguish repeated chrome updates from
process CPU noise. The analyzer additionally separates Chromium GPU, renderer,
browser-main and audio-service processes. Process private bytes are not VRAM;
optional Chromium JS heap counters are separate, approximate diagnostics. Use
identical observer instrumentation in both variants.
