# OSF UI 2.1

Release candidate; not published. Install one complete edition and exit Starfield before updating or switching editions.

## Editions and requirements

- **WebView2** is the recommended default and requires Microsoft Edge WebView2 Evergreen Runtime.
- **CEF** includes its Chromium runtime and does not require WebView2.
- Both require SFSE, Address Library, and **OSF Settings 1.1.2 or a compatible newer release**. This is the supported dependency baseline; older Settings builds have not been qualified against the current candidates.

Mods use the same public APIs with either edition. Native API 2.0, page bridge protocol 2, and the documented 1.6 compatibility interfaces remain unchanged. Private game-to-helper protocol 5 requires matching DLL and helper files; reinstall the entire edition if a mismatch is reported.

## Changes

- Added a CEF edition with accelerated off-screen rendering and bundled browser files.
- Improved texture copying and frame scheduling; performance varies by workload. See [browser editions](browser-backends.md) for measured tradeoffs.
- Fixed repeated hotkey releases and retained menu frames during the Settings launcher handoff.
- Kept CEF view-message queue overflow local to the affected view; excess pending messages drop oldest entries with a diagnostic.
- Allowed healthy CEF views to keep updating during sibling reloads or renderer failures. Partial frames cannot complete a menu preparation or reveal.
- Added bounded recovery for transient CEF graphics and WebView2 graphics-initialization failures, while retaining permanent errors for incompatible installations.
- Selected the game adapter for Chromium and allowed graceful helper shutdown before forced termination.
- Aligned page-message limits at 64 KiB and 128 messages per second. CEF supports local data/blob fetches, HEAD requests, and single byte-range asset requests.
- Release packages omit PDB files by default. Private debugging packages can opt in with `-IncludePdb`.

## Validation status

Current native, build and helper-runtime results are recorded in the [qualification ledger](release-2.1-qualification.md). CEF passed adapter selection on an RTX 3080 and AMD integrated GPU, storage persistence across shutdown, PCM media seeking, and view isolation through loading failures and renderer crashes. Those checks found and corrected HEAD and byte-range response bugs. Fresh Starfield acceptance, other hardware and video codecs remain to be checked.

The CEF edition includes `CEF-LICENSE.txt` and Chromium third-party `CREDITS.html`. Thanks to the CEF and Chromium contributors.
