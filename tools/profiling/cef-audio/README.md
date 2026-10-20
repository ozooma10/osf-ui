# CEF raw PCM and positional TV prototype

Private feasibility experiment, compiled only with `OSFUI_CEF_AUDIO_PROBE=ON`.
The ordinary CEF target defaults to OFF; WebView2 is unchanged.

## What it does

`CefAudioHandler` receives 48 kHz stereo planar floats from the reserved browser
pair. The prototype downmixes to mono, copies into a bounded queue, and plays
through an independent XAudio2 source voice. X3DAudio supplies the speaker matrix
from the TV and listener positions. CEF suppresses its original audio output when
the callback is enabled; no Windows session or endpoint mute is needed.

The private `OSFCEFAudioProbe.psc` fixture reads the real TV reference and player
position/yaw through Papyrus natives. It sends positions through OSF UI's existing
native event path. No game objects are read from the audio thread. Only the
compile-time prototype host recognizes the fixture event; remote content receives
no bridge, added script, or control channel.

The listener approximates the player's head (+1.6 m) and yaw. Coordinates map
Starfield XYZ to X3DAudio XZY. The emitter is omnidirectional, with full volume
through 1 m and linear falloff to zero at 20 m. World position updates older than
two seconds silence the source. Watch pauses the VM, so fullscreen retains the
last valid pose; closing resumes live updates. Browser mute also mutes the source.

Each stream has eight incoming and eight submitted buffers. The callback copies
samples only; a separate worker handles XAudio2, spatial calculations and evidence
files. Overflow drops the oldest pending packet. Stream restart discards old
buffers, and voice destruction completes callbacks before storage is released.

This is **not game sound-engine integration**. It has no Wwise room acoustics,
occlusion, game-volume routing, Doppler, device-change recovery, final camera
orientation, or multiple-TV emitter policy. Browser stereo is deliberately
downmixed. Production support needs those decisions, native frame pose updates
and lifecycle handling; this experiment is not enabled in a normal mod install.

## Build and reproduce

```powershell
pwsh tools/build-cef-host.ps1 -AudioProbe
pwsh tools/profiling/browser-audio/build.ps1
```

The private runtime is `build/cef-audio-host/runtime`; the ordinary build remains
`build/cef-host/runtime`. Both have their own generated runtime manifest. Stage
the private runtime with a matching CEF core in a disposable MO2 profile/save,
using the existing native consumer and private Settings consent mapping.
Do not replace the normal WebView2 mod.

Compile `OSFCEFAudioProbe.psc` against `data/Scripts/Source` and the vanilla Papyrus
imports. After spawning/binding the real TV, run through the native command mailbox:

```text
cgf "OSFCEFAudioProbe.Track" <signed-reference-form-id> 300 1.0
```

Start tracking while gameplay is running and confirm a current pose before opening
Watch. All three arguments are supplied explicitly. The private trusted chrome
can use `../browser-audio/tone.js` for the quiet 997 Hz control signal. Test ordinary
remote playback with the public
[Sintel WebM trailer](https://media.w3.org/2010/05/sintel/trailer.webm).

Host evidence is written to `audio-probe-{tv,content}.json` in that surface's CEF
profile. Check its PID and tick against the owned host. `inputEnergy`/`frames`
deltas measure PCM energy during a particular phase; `inputRms` is cumulative.
The native-only `cef-audio-output` event, with `{enabled:false}` / `true`, switches
the replacement source without affecting capture or Chromium's volume.

Read-only output measurements:

```powershell
build/browser-audio-probe/Release/osfui_browser_audio_probe.exe `
  --cef-meter --pid <owned-cef-host-pid> --start-time <creation-FILETIME> `
  --output <fresh-directory> --phase-ms 3000 --tone-hz 997 --wav
```

This mode verifies the CEF executable and process creation time, captures only the
owned host tree for WAV output, and reports aggregate endpoint metrics separately.
It never obtains or changes session volume. Exit 0 means measurement completed;
the scenario must evaluate energy, direction, freshness and continuity itself.
Use native `player.setpos` / `player.setangle` commands, observing the pose that
actually reaches the audio host before measuring.

## Runtime findings — 2026-10-20

The direct-command experiment established useful CEF PCM capture, replacement
output without duplicate sound, position-based panning, turning response, distance
falloff, and unmodified remote-video playback in Starfield 1.16.244.0 with CEF
150.0.22. The Windows output device reports eight channels; the read-only meter
records its stereo downmix, so measured channel ratios include device processing.

The first run also exposed a real CEF startup bug: synchronous browser creation
could run before the global or isolated request context was ready. The host now
starts its ordered pipe reader only after context initialization and successful
remote preference setup. Failure still closes the host. This fix also applies to
the ordinary CEF build.

Early iterations are retained under `build/cef-audio-game/runs/`, including
`20261020-155306-179-cef-audio`. That iteration verified spatial output but is marked
failed because fullscreen pause triggered stale-pose silence. The final prototype
retains the last valid pose during fullscreen.

The final run is
`build/cef-audio-game/runs/20261020-155921-244-cef-audio/`.
Its `audio-results.json` records 14 passing audio/continuity checks. Representative
process output measurements (linear RMS):

| Case | Left | Right | Raw mono PCM |
| --- | ---: | ---: | ---: |
| TV ahead, 2.5 m | 0.022074 | 0.021993 | 0.021214 |
| TV to the right | 0.006689 | 0.018091 | 0.021213 |
| TV to the left | 0.015995 | 0.007909 | 0.021213 |
| Same location, turn 180 degrees | 0.006850 | 0.014501 | 0.021213 |
| TV ahead, 17.5 m | 0.003154 | 0.003142 | 0.021213 |
| Replacement output disabled | 0.00001461 | 0.00001463 | 0.021213 |

The disabled-output measurement is at the 16-bit conversion noise floor; its
997 Hz energy is below 0.00000008 RMS. Endpoint tone measurement also confirms
suppression. Returning to 2.5 m restores the original level; the measured
near/far difference is 16.9 dB. No callback packets were dropped.

Actual remote-video capture/output continued through fullscreen pause and for at
least five seconds after `browser.close`, with the same stream generation, URL
and trusted document. No physical Esc key was injected. A native background WGC
capture records the video displayed on the world TV.

`tv-unload-final.json` verifies a fresh content stream after re-enabling the TV,
then normal host exit 10.625 seconds after disabling it. The first ten-second wait
expired at the existing feed grace boundary; its record is retained, and the
repeat uses a fifteen-second deadline. Normal game exit and Default-profile
restoration complete cleanup.

Both CEF host variants and the private CEF core build successfully; all 33 native
suites pass, with the existing symlink-permission skip. `build/cef-audio-*.log`
contains build/native logs. The normal WebView2 core/host and World Screens DLL
hashes still match `build/browser-tv-framework-deployment.json`.

References:
[CEF audio callbacks](https://github.com/chromiumembedded/cef/blob/master/include/cef_audio_handler.h),
[CEF browser creation](https://github.com/chromiumembedded/cef/blob/master/libcef/browser/browser_host_create.cc),
[X3DAudio with XAudio2](https://learn.microsoft.com/en-us/windows/win32/xaudio2/how-to--integrate-x3daudio-with-xaudio2).
