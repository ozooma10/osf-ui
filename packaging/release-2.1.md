# OSF UI 2.1 release preparation — October 22, 2026

Both editions require **OSF Settings 1.2.0 or a compatible newer release**, SFSE,
Address Library, and Starfield 1.16.244.0. WebView2 additionally requires Evergreen;
CEF bundles its runtime. Publication, uploads, and tags are separate actions.

## Source reconciliation

The previously deployed WebView2 helper was built from the changes now committed
as `6bcbfb23c391edeb263a879c7eaa1ea840ef440c` on
`oz/native-capture-diagnostics`. The `dev` branch has incorporated that commit by
fast-forward. It restores the native F11 capture/publication diagnostics and WGC
timing guard to the source used for this release.

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

Fresh archives and final validation are in preparation. Do not treat the
[October 19 release record](release-2.1-2026-10-19.md) as acceptance of this source.
That record is historical and used OSF Settings 1.1.2. Updated package hashes,
source revisions, the production 1.2.0 dependency artifact, and actual test
results will be recorded here before release qualification.
