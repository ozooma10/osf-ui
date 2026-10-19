# OSF UI 2.1 release record

Local release preparation only. Publication, tags, and uploads require separate
authorization. Public author documentation starts at [the authoring guide](../docs/authoring.md);
Nexus drafts are in [nexus](nexus/release-2.1-fields.txt).

## Production dependency

Both editions require OSF Settings 1.1.2 or a compatible newer release. Acceptance
uses this exact production artifact; the version label alone is insufficient:

| Field | Value |
| --- | --- |
| Archive | `OSFSettings-1.1.2-release.zip` |
| Build directory | `OSFSettings-1.1.2-release-20261013-190150-17b69514` |
| Source | `6de2876bab663f6a515e99dedf7ac294bbb9942a` |
| Configuration | `windows/x64/releasedbg; test_harness=n` |
| Archive SHA256 | `EF5183DCBD6A72D7871780FF10EA1FA09621054913AE31F8A7F8B2137788E44A` |
| Loaded DLL SHA256 | `1EC8BADE3AB66C86CBF3F0B82D1B37A998AB80FFF80DC6FD794084F463F091AD` |

The five SDK headers at that source match `sdk/vendor` after line-ending
normalization. This records a tested dependency, not the publication status of
any companion release.

## Final archives and acceptance

Final archive generation and WebView2 game regression are pending. Build both
editions from the same clean revision with `pwsh tools/package.ps1 -Tag ''`.
Expected names are `OSF-UI-v2.1.0-webview2.zip` and `OSF-UI-v2.1.0-cef.zip`, each
with a SHA256 sidecar and a manifest containing the source revision and payload
hashes. Public archives omit PDBs.

The [game suite](../tests/game/README.md) records the completed CEF checks:
68 assertions passed in two fresh Starfield processes, covering GPU selection,
decoded VP8/WebM seeking, sibling-view isolation/recovery, storage persistence,
and normal shutdown. That run used the earlier archive at source `3c2b849`; it
does not by itself validate a replacement archive.

Acceptance is scoped to Starfield 1.16.244.0, stock graphics, and the tested RTX
3080 system. Luma/HDR/frame generation, physical controllers, IME, other codecs,
other hardware, and all historical third-party integrations remain outside this
release check. Performance comparisons do not promise a general CEF advantage.
