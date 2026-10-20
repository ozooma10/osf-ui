# WebView2 world-screen audio validation

Status: 2026-10-20. Run **`20261020-202412-627-webview2-spatial`** records
**211 passing checks, zero failures, zero missing phases and two conditional
geometry skips** after the controller-mode and direct-media navigation fixes.
Controlled-tone spatial behavior and real public WebM/MP4 playback are verified
in the game host. Final-payload collector-crash recovery, save/load rebinding,
cell retirement and cleanup also passed. **The tested host is deployed to the
normal OSF UI mod with matching hashes.** Earlier results remain below with
their provenance.

The [final-run assertions](../../../build/webview2-spatial-game/runs/20261020-202412-627-webview2-spatial/audio-results.json)
retain measured values, input hashes and skips. Evidence under `build/` is local;
a later source change requires matching build/run evidence.

## Final-run environment and payload

- Starfield **1.16.244.0**, WebView2 Runtime **155.0.4283.62**.
- Disposable MO2 mod/profile
  `OSF WebView2 Spatial Test 20261020-202412-627-webview2-spatial`, with private
  saves and Settings values. Only trusted chrome contains the test oscillator
  and observer. Remote websites receive no injected script or native bridge.
- Aggregate endpoint measurements at **48 kHz, stereo, channel mask 3**. They
  retain numeric levels rather than PCM recordings. Ordinary game/desktop audio
  can contribute to these measurements.
- Native mailbox commands exercise game state and browser controls. Watch/close
  evidence does not establish physical Esc, keyboard or controller acceptance.

Hashes read from this run's private payload match its retained
[payload manifest](../../../build/webview2-spatial-game/runs/20261020-202412-627-webview2-spatial/private-payload-sha256.json).
The [host-source manifest](../../../build/webview2-spatial-game/runs/20261020-202412-627-webview2-spatial/host-source-sha256.json)
records the source snapshot separately. These are not a claim about a later
rebuild; the matching normal-mod deployment is recorded below.

| Private payload file | SHA-256 |
| --- | --- |
| `SFSE/Plugins/OSFUI.dll` | `41DBC0678102345B92ECF44F4A0A354B3604A13FDA3732B79051D60E0315DFA3` |
| `SFSE/Plugins/OSF/UI/bin/osfui_webview2_host.exe` | `C5F5FCCE56DF66A3FB57C8D66CE43DF9C58FFBAC2BFF6F0BE4E999724276B41B` |

The final native [media-navigation test](../../../tests/native/wv2_media_navigation_tests.cpp)
passes **103 checks, zero failures** for the exact status gate, navigation/DOM
identity, deadline and top-level media metadata validation. The production
[extension tests](../../../tests/native/webview2_audio_extension_tests.cjs) pass
**10/10**, covering capture/lifecycle ordering, coordinates, expiry, normalization,
mute and stale completion with mocked browser/audio APIs. These focused tests
are separate from the game-host measurements below; they do not establish real
runtime capture or endpoint behavior by themselves.

## Final-run tone and video results

[analyze.py](analyze.py) checks endpoint health, device/format consistency,
surrounding source/native observations, scene freshness and expected phase
behavior. The **211/0/0/2** result includes 17 controlled-tone phases and three
video measurement windows. The additional MP4 measurements below are separately
reviewed observations; they are listed as unreviewed phases by that analyzer.

| Controlled-tone observation | Measured result |
| --- | --- |
| Camera-relative panning | Approximately −3/+3 m along the actual camera's right vector; favored-channel tone ratios **2.12347× / 2.12024×**. |
| Master volume | Half gain produced **0.507597×** tone RMS; restoring gain produced **1.014079×**. |
| Distance fade | Distant single-TV tone was **0.308151×** the near level. This establishes attenuation, not an exact curve fit when HRTF bearing changes. |
| Mute, no TVs, out of range | Tone RMS at most **6.06e-12**, against the audible reference **0.0185112**; the source oscillator continued advancing. |
| Camera and multiple TVs | Listener independence from player movement and **2 → 1 → 0 → 2** emitter lifecycle checks passed. |
| Tone Watch/close | Fullscreen, interaction and pause transitions passed while preserving the source document and acknowledged running tone. |

The two conditional skips are the fixed-geometry tone-amplitude comparisons for
`fullscreen` and `after-close`. The camera basis/geometry did not match the
reference closely enough to isolate a presentation effect. Their observed ratios,
**1.011761×** and **1.081794×**, remain observations, not amplitude-continuity passes.
Audibility and source/presentation continuity passed independently.

Direct navigation to the unmodified public
[WebM trailer](https://media.w3.org/2010/05/sintel/trailer.webm) reached a fresh
`capturing` content route with document audio activity while the private tone was
closed. The same trusted controller document was observed around all three
measurement windows:

| Video window | Fullscreen before → after | Endpoint RMS |
| --- | --- | --- |
| `video-world` | false → true | **0.0939818** |
| `video-fullscreen` | true → false | **0.126584** |
| `video-after-close` | false → false | **0.170241** |

The first two windows contain presentation changes. They establish measured audio
**through the observed Watch/close transitions**, not strictly isolated world-only
or fullscreen-only intervals. The final window measures continued playback after
close. Changing soundtrack content prevents meaningful cross-window gain ratios;
page/native snapshots do not prove every intervening audio frame was gap-free.

Direct [MP4 trailer](https://media.w3.org/2010/05/sintel/trailer.mp4) playback also
reached `capturing`: endpoint RMS **0.0374622**, falling to **1.29e-8** with logical
mute and returning to **0.183143** after unmute. These healthy meter observations
support playback and mute/recovery; their nonstationary levels are not a gain-ratio
comparison. This does not qualify arbitrary sites, iframe players or DRM.

## Fixes exposed by game integration

The initial runs `20261020-195808-419-webview2-spatial` and
`20261020-200514-805-webview2-spatial` failed before usable capture. The latter
records `CreateCoreWebView2Controller` completion failing with **`0x8007139F`
(`ERROR_INVALID_STATE`)**. The standalone source used ordinary controllers; the
actual host uses composition controllers. Hidden collectors now match their
source environment's controller mode.

Run `20261020-200741-939-webview2-spatial` then passed **190 tone checks, zero
failures and two conditional geometry skips**, but retained an overall failed
outcome because direct media did not start the content route. Diagnostic run
`20261020-201738-501-webview2-spatial` identified
`NavigationCompleted(success=false, webErrorStatus=9, httpStatus=200)` even though
media committed and reported document audio activity. Status 9 is
`CONNECTION_ABORTED`.

Chromium's [DocumentLoader implementation](https://github.com/chromium/chromium/blob/main/third_party/blink/renderer/core/loader/document_loader.cc)
explains this case: after committing a media document it finishes parsing and
stops the original request; the generated media element requests the resource
separately. This does not justify accepting every failed HTTP 200 navigation.

The final-run fix accepts only remote HTTP 200 / `CONNECTION_ABORTED` completions
whose exact navigation ID receives `DOMContentLoaded` and whose browser-owned
`Page.getFrameTree` metadata identifies a matching top-level audio/video document.
It validates controller/navigation identity, current source, frame/loader IDs,
URL including fragment and absence of an error-page URL. A two-second deadline,
one outstanding metadata request per controller and stale-reply rejection bound
the check. Native source mute remains enabled; confirmed media releases the
existing spatial route's loading gate. No remote-page script is injected.

## Final-run crash, lifecycle and cleanup

These checks use the final payload above and are separate from the 211 analyzer
checks. The process tests identify the owned collector by its command line and
creation identity, then compare host/browser/source-renderer identities after
recovery.

- [Active-media collector crash](../../../build/webview2-spatial-game/runs/20261020-202412-627-webview2-spatial/collector-crash-active-media/result.json):
  the extension renderer was terminated while the MP4 and trusted chrome tone
  were active. Both routes recovered in **1.2705089 s** with source processes,
  document and tone acknowledgement preserved. Post-recovery endpoint RMS was
  **0.136368** with the tone and video playing together. Native logs record both
  collector failures; the 500 ms observer missed the brief failed state. This
  establishes recovery, not a gap-free crash interval or exact transient length.
- [Blank-source collector crash](../../../build/webview2-spatial-game/runs/20261020-202412-627-webview2-spatial/collector-crash-blank/result.json):
  the active trusted tone recovered in **0.8805904 s** with source identities
  preserved; tone RMS afterward was **0.00933037**. The inactive `about:blank`
  content collector correctly remained `starting`, waiting for a real URL.
- The [first crash attempt](../../../build/webview2-spatial-game/runs/20261020-202412-627-webview2-spatial/collector-crash/result.json)
  retains `recovery-check-failed`: after MP4-to-home navigation the helper
  incorrectly required the inactive blank content source to resume capture. The
  trusted tone recovered, and subsequent MP4 navigation captured successfully
  with endpoint RMS **0.0388341**. The helper was corrected to distinguish active
  media from an idle blank source; both explicit reruns above passed. This is a
  retained helper false failure, not an erased result or an unexplained pass.
- [Save/load](../../../build/webview2-spatial-game/runs/20261020-202412-627-webview2-spatial/save-load-audio-confirmed.json):
  a newly created, readable private checkpoint was hashed and loaded by exact
  name. In **3.839 s**, document `ded4133a-0005-4456-b452-31dc6a1f92e3` replaced
  `b9f331d8-8834-414f-804c-86f219ef8299` and binding IDs **3/4** replaced **1/2**.
  The intentionally restarted fixture tone reached `capturing`; post-load tone
  RMS **0.00933058** confirms restored output. JavaScript continuity across load
  is not claimed.
- [Cell retirement](../../../build/webview2-spatial-game/runs/20261020-202412-627-webview2-spatial/cell-unload-audio.json):
  transfer from cell **2876329** to **1281796** completed at **6.922 s** with zero
  pause requests. The pinned prior TV host, PID **10612**, was confirmed exited
  at **6.923 s**, without forced host termination. The post-transfer aggregate
  endpoint remained audible: RMS **0.0800104**, 997 Hz band RMS **0.000536934**.
  Other game sound can contribute; completed host exit proves retirement,
  while the endpoint measurement does not establish silence.
- The owned game exited normally through `qqq`. The [final run report](../../../build/webview2-spatial-game/runs/20261020-202412-627-webview2-spatial/report.md)
  is **passed** with no evidence warnings. The [browser-profile lease](../../../build/webview2-spatial-game/runs/20261020-202412-627-webview2-spatial/browser-profile-lease.json)
  records **`restored: true`** after original-profile file-hash verification;
  MO2 returned to **Default**. Test saves, tone fixtures and test Settings values
  remain confined to private test directories.

Earlier lifecycle evidence remains in
`20261020-200741-939-webview2-spatial`: collector recovery **0.8455989 s**, save/load
**3.668 s**, cell retirement **7.431 s**, and a restored browser-profile lease.
Its core hash was the same, but its host hash was
`5F4E2747355D843EDF3B1D782C4767B79B77D6EFBBFDDA8A31CECCC28FD2DF88`.
That run's overall failed outcome from the unresolved direct-media route is
retained; the final run supplies the repeated lifecycle and media evidence.

## Deployment and remaining limits

The [deployment receipt](../../../build/webview2-spatial-game/runs/20261020-202412-627-webview2-spatial/deployment.json)
records the tested host copied to the normal
`MO2/mods/OSF UI/SFSE/Plugins/OSF/UI/bin/osfui_webview2_host.exe`, with its SHA-256
matching **`C5F5FCCE56DF66A3FB57C8D66CE43DF9C58FFBAC2BFF6F0BE4E999724276B41B`**.
Only that host file changed. The prior host, hash
`4A513D0228D677F4CB5D05A3A39755F38FFCF6B4C1F127AAB277A1CD9A97A2F5`, was backed
up under the run's `deployment-backup/`. The unchanged normal core matches the
tested core hash above. All **42** recorded host-source hashes matched with
**zero source drift**. Deployment followed normal game exit and browser-profile
restoration; it does not extend this run's acceptance to untested scenarios.

Endpoint switching/removal, suspend/resume, subjective sound quality,
latency/video synchronization, physical input and broad hardware coverage remain
unverified. The Chromium exact-extension allowlist remains a runtime compatibility
dependency. Public media playback does not establish compatibility with all
websites or protected media.
