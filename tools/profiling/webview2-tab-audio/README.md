# WebView2 extension capture and spatial audio probe

## Result — 2026-10-20

**Works on WebView2 155.0.4283.62 on this Windows 11 installation**, using an
exact-extension Chromium allowlist flag. The standalone harness remains outside
shipping build/package targets. The route is now integrated into the production
WebView2 host; `-ProductionRoute` compiles that actual route and its embedded
extension assets into this isolated test executable.

The prototype installs one bundled extension into a fresh private profile and
creates two hidden WebViews:

1. A source fixture plays a quiet 997 Hz Web Audio tone, or a remote MP4 inside
   a normal HTML video element.
2. A trusted extension page obtains the source tab's audio with
   `chrome.tabCapture.getMediaStreamId({targetTabId})` and `getUserMedia`.
3. The captured stream feeds a mono Web Audio `PannerNode` using HRTF and inverse
   distance attenuation. The collector can silence its own output while retaining
   capture, then replay at center, left, right, or 20 metres.

The source tab's original output is suppressed by tab capture. This avoids the
failure of [Windows session mute plus process loopback](../browser-audio/README.md):
the captured stream remains audible even when nothing is sent to the speakers.
Audio stays inside the browser's Web Audio graph; this probe does **not** transfer
PCM to the native CEF/XAudio2 player.

### Permission requirement

A normal launch exposes the APIs and enumerates both tabs, but rejects capture:

> Extension has not been invoked for the current page (see activeTab permission).

The tested successful launch adds:

```text
--allowlisted-extension-id=nbligbkobhajkimckkfnadnghkimjcld
```

That is the fixed ID derived from this extension's public manifest key. This is
an upstream Chromium switch, **not a documented WebView2 compatibility guarantee**.
The probe confirms that the installed runtime honors it; future Evergreen
versions still need verification. Declaring `activeTab` alone is insufficient.
The optional extension keyboard command was not exercised; all runs used native
commands without foreground transfer, mouse input, or keyboard injection.

### Measurements

The initial prototype controlled-tone run passed **49 assertions, zero failures**.
Numbers below are linear RMS from the default stereo render endpoint, with a
997 Hz narrow-band measurement to distinguish the fixture from unrelated audio.

| Phase | Endpoint tone RMS | Captured stream RMS |
| --- | ---: | ---: |
| Original browser output | 0.0246864 | — |
| Capture active, collector output silent | 0.00000000000951 | 0.0141377 |
| Spatial center, 1 m | 0.0135663 | 0.0141485 |
| Spatial far, 20 m | 0.00068551 | 0.0141381 |
| Capture stopped, original restored | 0.0247860 | Not measured |

Left positioning favored the left channel by about **1.645×**; right positioning
favored the right channel by **1.653×**. Far/center amplitude was **0.0505**,
consistent with the configured 1-to-20-metre inverse-distance curve. Restoration
was **1.004×** the original level. Active measured phases had no discontinuities
or capture errors.

The remote-video run played the public
[Sintel trailer](https://media.w3.org/2010/05/sintel/trailer.mp4) in the controlled
fixture, with no CORS attribute or page-to-game bridge. The video kept advancing
while captured RMS was nonzero and silent-collector endpoint RMS fell to
`6.62e-10`. Directional playback favored the expected channel. **47 assertions
passed; two quantitative comparisons were skipped** because the soundtrack
changes between phases. Distance and restoration-amplitude proof comes from the
stationary tone, not that video.

This establishes remote **media-element playback** capture. It does not establish
arbitrary remote sites, cross-origin iframe players, DRM, or every codec.

### Evidence

Initial prototype runs, all with the same executable and recorded source hashes:

- [Controlled tone: result](../../../build/webview2-tab-audio-probe/runs/20261020-190835-150-allowlist/result.json)
  and [49 passing assertions](../../../build/webview2-tab-audio-probe/runs/20261020-190835-150-allowlist/assertions.json).
- [Remote MP4 fixture: result](../../../build/webview2-tab-audio-probe/runs/20261020-190750-287-allowlist-video/result.json)
  and [47 passing assertions, two skips](../../../build/webview2-tab-audio-probe/runs/20261020-190750-287-allowlist-video/assertions.json).
- [Normal launch: expected invocation-grant rejection](../../../build/webview2-tab-audio-probe/runs/20261020-190925-112-normal/result.json).

Each run has `provenance.json`, recording executable and source SHA-256 hashes.
Initial prototype executable SHA-256:
`6B977664034F40CEA96DA5D9304F6502507CA15CC682A4D64C0D6CB39F5EC865`.
The MSVC Release build completed without warnings.

Earlier attempts navigating directly to the MP4 URL failed before capture with
WebView2 `CONNECTION_ABORTED`; those infrastructure failures remain in the runs
directory. The successful video test uses an HTML video element in the source
fixture. An initial sandboxed network check also encountered DNS failure.

## Reproduce

From the repository root:

```powershell
pwsh tools/profiling/webview2-tab-audio/build.ps1

# Expected negative control: native process exit 2, capture-rejected.
pwsh tools/profiling/webview2-tab-audio/run.ps1

# Controlled tone, suppression, HRTF direction, distance, restoration.
pwsh tools/profiling/webview2-tab-audio/run.ps1 -Allowlist

# Requires network access to the public W3C media host.
pwsh tools/profiling/webview2-tab-audio/run.ps1 -Allowlist -RemoteVideo

# Prove native source mute preserves captured PCM and prevents output on stop.
pwsh tools/profiling/webview2-tab-audio/run.ps1 -Allowlist -NativeSourceMute

# Exercise actual shipping route code, assets, scenes, and lifecycle guards.
pwsh tools/profiling/webview2-tab-audio/run.ps1 -Allowlist -ProductionRoute

# Analyze the result.json path printed by a successful run.
node tools/profiling/webview2-tab-audio/analyze.mjs <result.json>
```

The executable returns 0 when all capture/playback phases complete, 2 for capture
rejection, or 1 for infrastructure failure. Completion alone is not measurement
acceptance: `analyze.mjs` writes `assertions.json`, preserves the raw report, and
returns nonzero for failed checks. Quiet idle endpoints may produce zero packets;
active phases must produce sufficient audio frames.

The probe uses a new profile under `build/webview2-tab-audio-probe/runs/` for every
run. It does not install a Windows driver, alter device/session volumes, inject
code into another process, or connect remote content to native host objects.
The original prototype disables web messages and host objects in both views.
The production harness source still disables both; its private trusted collector
enables web messages for native status reporting, while disabling host objects.
Device permission requests and downloads are denied. The meter retains numeric aggregate measurements only,
not desktop audio recordings. Keep the output device stable and avoid unrelated
audio during a run.

## Native mute guard and production route

[SpatialAudio](../../webview2_host/SpatialAudio.cpp) owns native source muting and
the private collector. [HostApp](../../webview2_host/HostApp.cpp) binds it to the
reserved browser views, and [BrowserHostClient](../../../src/Render/BrowserHostClient.cpp)
delivers shared scenes. The embedded [production collector](../../webview2_host/audio-extension/collector.js)
uses HRTF, a linear 1–20 metre distance curve, bounded multiple emitters, logical
mute, and scene deadlines. These differ from the original inverse-distance probe.

The source is natively muted **before its first navigation and remains muted**.
Logical user mute controls the replacement route. A stopped or crashed collector
therefore cannot restore audible non-positional source output. The separate
native-mute experiment directly proved capture survives both enabling source
mute and starting a new capture while the source is already muted:

- [Native mute: result](../../../build/webview2-tab-audio-probe/runs/20261020-192808-693-allowlist-native-mute/result.json)
  and [79 passing assertions](../../../build/webview2-tab-audio-probe/runs/20261020-192808-693-allowlist-native-mute/assertions.json).
- Captured RMS remained about `0.01414`. Stopping either capture while native
  mute remained enabled produced no endpoint packets; restoring native mute to
  its original false value restored the original tone.

The production harness measures the shipping C++ manager and byte-identical
embedded extension assets. Build provenance records source and asset hashes,
rejects inputs that change during compilation, and verifies the executable and
compiled-input hashes again before launch. It exercises a 16-second idle
`about:blank` start, real scene updates, left/right/far output, logical mute,
empty/expired scenes, recovery, retained capture across source navigation,
failure cleanup, and destruction/recreation after navigation.

The final production run passed **99 assertions, zero failures, zero skips** on
WebView2 `155.0.4283.62`:

- [Production result](../../../build/webview2-tab-audio-probe/runs/20261020-193522-318-allowlist-production/result.json),
  [99 passing assertions](../../../build/webview2-tab-audio-probe/runs/20261020-193522-318-allowlist-production/assertions.json),
  and [binary/compiled-input provenance](../../../build/webview2-tab-audio-probe/runs/20261020-193522-318-allowlist-production/provenance.json).
- Idle `about:blank` remained healthy for `16.109` seconds before first navigation.
  Native source mute read back true in all fifteen phases.
- Left/right channel dominance was `1.645×` / `1.653×`; the 19-metre output was
  `5.34%` of the 1-metre center level.
- Logical mute, empty scenes, and expired scenes reduced measured tone to
  `1.47e-10` or less. Failure and route destruction produced zero endpoint packets.
- Keeping capture across source navigation restored center to `101.31%`; a new
  route/controller after navigation reached `100.84%` of the original center.

The standalone checks do not exercise the complete game/HostApp integration,
engine listener transforms, subjective quality, latency/video synchronization,
output-device changes, suspend/resume, DRM, or arbitrary iframe players. Failure
injection calls the real manager's `Fail` path; it does not terminate a renderer.
The Chromium allowlist switch still needs verification after runtime updates.

Subsequent [in-game validation](../webview2-game-audio/VALIDATION.md) covers the
composition-controller host, measured scene behavior, remote media playback,
actual collector-renderer crashes and game lifecycle on the revised payload.

References:
[WebView2 extension installation](https://learn.microsoft.com/en-us/microsoft-edge/webview2/reference/win32/icorewebview2profile7),
[Chrome tab capture](https://developer.chrome.com/docs/extensions/reference/api/tabCapture),
[Chromium capture permission check](https://raw.githubusercontent.com/chromium/chromium/main/chrome/browser/extensions/api/tab_capture/tab_capture_api.cc).
