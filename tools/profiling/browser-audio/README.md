# WebView2 capture and suppression probe

Standalone Windows feasibility experiment. Nothing here is packaged, deployed
into a normal mod, or linked into the shipping browser host.

The prototype tests whether Windows process loopback can capture useful PCM from
the TV browser while `ISimpleAudioVolume::SetMute` suppresses its ordinary output.
It does not implement positional playback.

## Result — 2026-10-20

**This combination does not work on the tested system.** Capturing WebView2's
audio-service process works, and session mute suppresses the original output,
but it also silences the captured PCM. Restoring the session recovers both.

The actual Browser TV was running in Starfield 1.16.244.0, with WebView2
155.0.4283.62 on Windows 11 build 26300. A private trusted-chrome fixture generated
a 997 Hz tone at gain 0.03. A second test played the public
[Sintel trailer](https://media.w3.org/2010/05/sintel/trailer.mp4)
in the unmodified remote content view. Both tests used direct native mailbox
commands; no computer use, focus transfer, or injected input was used.

Representative measurements from the first audio-service runs (linear RMS,
normalized to full scale; three seconds per phase):

| Source | Normal capture | Muted capture | Restored capture |
| --- | ---: | ---: | ---: |
| 997 Hz tone | 0.024720 | 0.00001462 | 0.024720 |
| Remote video | 0.039330 | 0.00001462 | 0.218609 |

The muted signal is approximately the 16-bit conversion noise floor, not useful
audio. The tone also disappears from endpoint loopback and returns on restoration.
Video RMS varies naturally with the soundtrack. All phases received 144,000
stereo frames at 48 kHz with no reported discontinuities. Browser state and the
trusted document remained alive throughout; native session readback confirmed
restoration to unmuted, volume 1.0.

An earlier attempt targeting the OSF host process tree produced silent capture
even before muting, despite audible endpoint output. Selecting the actual
host-owned WebView2 audio-session PID resolved capture on this installation.
This is observed behavior, not a general claim about Windows process ancestry.

Local evidence is retained under
`build/browser-audio-game/runs/20261020-152108-008-browser-audio/`:
`audio-tone-112315222` (host tree), `audio-tone-112414694` (audio service), and
`audio-video-112506588` (remote video). Each contains native state observations,
`result.json`, and process-only WAVs. Failed feasibility checks are deliberately
retained as failures. The final warning-free build repeated both checks in
`audio-tone-112757095` and `audio-video-112810654`, with the same negative result
and successful mute restoration. `final-build-results.json` records its binary hash.

## Build and run

```powershell
pwsh tools/profiling/browser-audio/build.ps1
# Obtain these from the owned game's exact surface host process.
build/browser-audio-probe/Release/osfui_browser_audio_probe.exe `
  --pid <host-pid> --start-time <creation-FILETIME> `
  --output <fresh-directory> --tone-hz 997 --wav
```

The build uses the repository's pinned native-test JSON headers and MSVC/Windows
SDK. Windows process loopback requires build 20348 or newer.

The target must be `osfui_webview2_host.exe` with the exact supplied creation
time. The probe enumerates active sessions on the default console render endpoint,
accepts only sessions owned by the host or its WebView2 descendants, and captures
the single audio-session process. It rejects ambiguous multiple audio processes,
already-muted/near-zero-volume baselines and absent sessions. `--capture-root`
repeats the initial host-tree experiment.

For a controlled tone, add `tone.js` only to the private trusted chrome fixture,
include `window.osfAudioProbeSnapshot()` in its existing native-consumer heartbeat,
and send `osfui-browser-audio.command` with `{id, action:"start"}` / `"stop"` through
the native test consumer. The remote view receives no script or bridge.
For an ordinary video use `--tone-hz 0`; that mode reports capture survival but
cannot establish output isolation from an arbitrary soundtrack.

The bounded sequence is normal → session mute → restored. Each phase excludes
350 ms of transition packets and measures three seconds by default
(`--phase-ms` permits 1–10 seconds). Ctrl+C requests graceful cancellation.
Normal completion and exceptions restore every changed session's original mute
state; force-killing the tool is unsupported. It never adjusts endpoint/master
volume, game audio, or unrelated sessions. It stores endpoint aggregate
measurements only; `--wav` saves only the selected browser process's PCM.
The endpoint must stay unchanged during the experiment. This is a fixed-session
probe, not production handling of device changes or new browser audio sessions.

Exit 0 means capture survived suppression (plus tone isolation when requested);
exit 2 means the measurement completed and rejected that hypothesis; exit 1 is an
infrastructure/argument error. See `restored` and `finalSessions` in the report.

The separate `--cef-meter` mode is read-only and accepts an owned
`osfui_cef_host.exe`. It measures left/right RMS and endpoint output without
enumerating or muting audio sessions. Exit 0 in this mode means measurement
completed, not that a feasibility assertion passed. See the
[CEF positional prototype](../cef-audio/README.md).

## Implication

Do not wire this mute-and-loopback path into production spatial audio: it would
feed silence to the 3D player. Capturing before the Windows session gain, or
routing to a separately controlled output, needs another implementation.
Muting the whole default endpoint would also silence game/desktop audio and is
not an acceptable substitute.

CEF's native PCM callback remains a separate candidate for a follow-up prototype.
No backend switch, device installation, virtual cable, or audio-service injection
was performed here.

API references: [Microsoft process-loopback sample](https://learn.microsoft.com/en-us/samples/microsoft/windows-classic-samples/applicationloopbackaudio-sample/),
[audio sessions](https://learn.microsoft.com/en-us/windows/win32/coreaudio/audio-sessions).
