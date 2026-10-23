# Incremental packaging

`tools/package.ps1` retains native outputs independently in
`build/package/native/webview2` and `build/package/native/cef`. Both editions still
run their package-content checks and archive hash verification. `-SkipBuild` uses
these package-specific outputs; build each edition here before using it.

`tools/build-papyrus.ps1` reuses a successful PEX only when all script/import and
compiler-directory contents match and the output hash is intact. Both editions
therefore share one compilation. `-Force` requests a fresh compilation.

Run `pwsh -NoProfile -File tests/package/test-workflow-cache.ps1` for the cache's
failure and invalidation regression checks. No game or browser is launched.
