# OSF UI 2.1 release preparation — October 22, 2026

**Status: candidates built; final game acceptance deferred by the author until
immediately before release. These archives are not yet qualified for publication.**

Both editions require **OSF Settings 1.2.0 or a compatible newer release**, SFSE,
Address Library, and Starfield 1.16.244.0. WebView2 additionally requires Evergreen;
CEF bundles its runtime. Publication, uploads, and tags are separate actions.

## Source reconciliation

The previously deployed WebView2 helper contained native diagnostics missing from
`dev`. The matching source was preserved in
`6bcbfb23c391edeb263a879c7eaa1ea840ef440c` on
`oz/native-capture-diagnostics`; `dev` incorporated that commit by fast-forward.
It restores the native F11 capture/publication diagnostics and WGC timing guard
to the source used for this release.

The subsequent `9ae09f2e` merge also incorporates the asynchronous WebView2 pipe
writer (`dec8b637`) and temporary test-profile registration (`0249d0e3`). Its
348 Windows pipe checks pass. An initial package attempt crossed that merge and
is retained under `build/release-2.1-20261022/superseded-mixed-revision-attempt/`;
those archives are rejected for release because their source revisions differ.

Release packaging uses `pwsh tools/package.ps1 -Backend both -Tag '' -RequireClean`.
The packager pins HEAD before building and rejects a revision change during
either edition. `-RequireClean` also checks the worktree and submodules and
disallows reusing unverified binaries with `-SkipBuild`.

The earlier tested helper SHA256 is
`9E2684ECC6B7B45C854DC8BDB825681FBB6425D79D52F750FE07948D5E38D3A7`.
Its embedded diagnostic script is compared with the reconciled source, and the
binary is preserved at `build/release-2.1-20261022/previously-tested-webview2-host.exe`.
The older report's statement that these changes were uncommitted predates the
commit above; it is retained as historical evidence.

F11 remains gated by the startup Developer mode setting, which defaults to false.
Neither backend injects the diagnostic script outside Developer mode. WebView2
also rejects direct native diagnostic requests outside Developer mode and from
remote content. With F11 off, the sampler is stopped. The WGC timing guard is a
normal rendering fix and is independent of diagnostic sampling.

## Candidate verification

Both archives were rebuilt from the same clean source revision,
`62146ae59ff33804ef9e631ace4d698fbe6b3a25`. Their adjacent `.zip.manifest.json`
files record `sourceDirty: false` and the per-file hashes. Later documentation
updates do not change the source revision of these binaries.

| Archive in `dist/` | Bytes | SHA256 |
| --- | ---: | --- |
| `OSF-UI-v2.1.0-webview2.zip` | 1,580,165 | `E6432A6E122B92F42386C59B3D69B2623B744D220343088276EC2E5032581182` |
| `OSF-UI-v2.1.0-cef.zip` | 185,283,413 | `597A90A4E1703013EF34A1FC179A29A1324A7549A385D8AE5927EE6593137907` |

Both release builds and package checks passed: exclusive backend payload,
55 Papyrus natives, matching shared scripts/assets, default-off Developer mode,
no PDBs, and verified ZIP payload hashes. All 10 package rejection checks passed.
The native/Papyrus game acceptance consumer also built successfully.

The production OSF Settings 1.2.0 acceptance dependency was built from clean source
`a8e3226c74a3fc7e04b458637691054887507573` with `test_harness=n`. Its archive is
`OSFSettings-1.2.0-osfui-2.1-acceptance.zip`, SHA256
`B3EEECF17AAF77B06DA4E8464538AEB5F571C90D20BA2A750A8544762FBCA2DF`.
The exact manifest path is retained in
`build/release-2.1-20261022/settings-manifest-path.txt`.

Fresh source checks passed: 39 portable native suites, all 20 CEF lifecycle
scenarios, 51 Windows capture-timing checks, 30 capture-counter checks, 34 direct
native diagnostic-gate checks, and 34 JavaScript tests. Two optional original-mod
fixture tests were skipped; Windows denied creation of a symlink for one escape
check. Test logs are under `build/release-2.1-20261022/`. Game fixtures now assert
that the diagnostic API/panel is absent outside Developer mode; the interactive
suite also verifies that F11 reaches the page without enabling it.

### Desktop first-frame diagnostic

The final helpers were tested at 2560×1440 with Developer mode off. WebView2
passed all three cold-open cycles and clean shutdown. **CEF failed** the strict
probe in `cold_second_1`: the first frame of epoch 2 was transparent
`[0, 0, 0, 0]`. Its normal shutdown passed. Evidence is retained under
`build/release-2.1-20261022/cold-open/`, including source and binary hashes.

This is an unresolved diagnostic failure, not a passed check. The
[probe's documented scope](../tools/profiling/backend-bench/README.md) excludes
the game's Settings preparation handshake and `SharedFrameState` queue, so it
does not establish whether the game shows a blank menu. The packaged CEF cold
launcher check must determine that before release; fix and rebuild if it fails.

### Game acceptance deferred

The attempted background CEF runtime run stopped **before game launch** because
the then-loaded shared harness lacked `New-TemporaryMO2Directory`. Its blocked
report is `build/backend-validation/game/20261022-151204-190-cef-stock-runtime/`.
The harness has since incorporated its cleanup module in `ccb0aeac`; a read-only
check confirms `Common.ps1` now loads the function. The game run was not repeated.
The author explicitly deferred game testing until immediately before release.

Before publication:

- Test the exact candidate ZIPs with the production OSF Settings 1.2.0 manifest.
  Cover both editions' input, focus, save/load, launcher handoff and cold opens,
  browser recovery, and the default-off F11 assertions. Record actual captures
  and outcomes using [the packaged acceptance guide](../tests/game/README.md).
- Run the focused CEF background runtime checks, and resolve or explicitly
  disposition the first-frame diagnostic based on the game acceptance evidence.
- Verify Developer mode after a restart: F11 opens/closes diagnostics, and
  turning Developer mode off followed by another restart removes them.
- Review the original consumer workflows and any supported Luma/HDR/controller
  configurations. Existing historical results do not qualify these binaries.
- If runtime source changes before release, rebuild both editions from one
  clean revision, update these hashes, and test the new archives.

The exact Settings manifest path is saved in the receipt above. For the synthetic
interactive runs, supply it and the explicit package paths (the runner otherwise
defaults to RC filenames):

```powershell
$settingsManifest = Get-Content build/release-2.1-20261022/settings-manifest-path.txt
pwsh tests/game/Test-Backend.ps1 -Backend webview2 -Interactive -Package dist/OSF-UI-v2.1.0-webview2.zip -SettingsManifest $settingsManifest
pwsh tests/game/Test-Backend.ps1 -Backend cef -Interactive -Package dist/OSF-UI-v2.1.0-cef.zip -SettingsManifest $settingsManifest
pwsh tests/game/Test-Backend.ps1 -Backend cef -RuntimeChecks -Package dist/OSF-UI-v2.1.0-cef.zip -SettingsManifest $settingsManifest -VideoFixture build/release-2.1-20261022/seek.webm
```

The retained video is a generated 20-second VP8 fixture, SHA256
`68209FE14091FE6427FD1686442F2D66838B33FDB02822A020FAD872010C4BEA`.

Do not treat the [October 19 release record](release-2.1-2026-10-19.md) as acceptance
of this source. That record is historical and used OSF Settings 1.1.2.
