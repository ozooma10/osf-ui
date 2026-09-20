# Slim integration verification — 2026-09-19

The UI checkout started clean at `294f9a0`. The Settings SDK now points to
`ozooma10/osf-settings-slim` commit `929074399b6f1c584ecefa2d481c9a8c073bd36f`.
The separate OSF Settings Slim checkout was not edited. Its upstream `main`
was verified through authenticated SSH to match this exact revision. Anonymous
HTTPS access returned "repository not found"; this local checkout uses the
existing `github-personal` SSH alias, while the tracked URL remains HTTPS.
CI requires an `OSF_DEPENDENCIES_TOKEN` secret with dependency read access unless
its default token already has that access. No credentials or secrets were created.

## Local results

| Check | Baseline | After migration |
| --- | --- | --- |
| Native runtime/API/input suites | 27 suites passed | 29 suites passed |
| Frontend typecheck/build/tests | 133 tests passed with the Node 26 flag below | 134 tests passed with the same flag |
| Shipped UI + development example schemas, using Slim's parser | Not present | Passed |
| JSON syntax, protocol/release version checks, JSON wrapper policy | Existing manifest title and wrapper-exclusion path disagreed with CI | Passed after correcting those metadata references |
| Whitespace / shell script syntax | — | Passed |

Commands:

```sh
bash tests/native/run.sh
bash tests/schema/run.sh
NODE_OPTIONS=--no-experimental-webstorage npm run verify
```

On this host, Node 26's experimental native storage shadows jsdom's storage and
causes the unmodified parser test fixture to fail. Disabling that feature makes
the baseline and final tests pass. CI remains on Node 22. Pre-existing BridgeApi/Papyrus signedness and aggregate-initializer warnings
remain; the obsolete two-argument Papyrus dispatcher and its unused-function
warning were removed with the old integration machinery.

The new native checks exercise missing/incompatible/unready providers, startup
read failures, native issue report/update/clear retries, other-owner isolation,
long issue IDs, block acquisition/release failures, and the example's actual
consumer through the real Views request queues. Lifecycle checks cover lazy
state retention and diagnostics pumping without a renderer. The frontend test
uses the actual example page script and bridge helper.

Schema checks compile Slim's actual schema parser. Portable fixtures use only
booleans and declarative hotkeys: the test stubs abort if string conversion or
native key lookup is entered. Windows CI has a separate schema target using the
Windows SDK. These checks do not validate the game's native binding engine.

## Outstanding platform verification

The configured OSF Windows SSH relay timed out, the direct hostname did not
resolve, and its configured LAN address was unreachable. Windows production
plugin/host builds, example-plugin linking, staged package ownership checks,
and in-game acceptance could not run from this host. CI is configured to build
the example and run schema checks alongside the existing Windows build jobs.

Release staging also requires `data/Scripts/OSFUI.pex` and
`data/Scripts/OSFUI_View.pex`. Neither file is in this checkout, and the existing
build does not compile the `.psc` sources. Supply matching compiled scripts from
the Papyrus build before packaging; the existing ownership checks deliberately
continue to reject an archive missing those public view-API assets.

Run the isolated in-game steps in
[the development example](../examples/settings-view/README.md#acceptance)
before release. This includes native hotkey persistence, focus/block release,
browser failure/recovery, and Settings usability without WebView2.
