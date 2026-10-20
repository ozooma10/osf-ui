# Read-only endpoint measurements

This small CLI reuses the validated `webview2-tab-audio/AudioMeter` implementation.
It measures the default render endpoint's native channels and a selected tone
component. It does not launch/control the game or browser, acquire the harness
lock, change audio volume/mute, or retain PCM/WAV recordings.

```powershell
pwsh tools/profiling/audio-endpoint-meter/build.ps1
pwsh tools/profiling/audio-endpoint-meter/measure.ps1 -OutputDirectory build/my-run/meter-center
```

`-DurationMs` defaults to 3000, `-WarmupMs` to 500, and `-ToneHz` to 997 (valid range
1..20000 Hz). For ordinary media, keep the default tone setting and evaluate the
always-available broadband `endpoint.rms`/`channelRms`; their
attribution requires a quiet-background baseline and contemporaneous source
status. Endpoint measurements include game/desktop audio. The narrow-band tone
measurement helps distinguish the controlled fixture from unrelated ambience.

The caller owns scenario control and should save fresh before/after page,
consumer, game, camera/emitter, and process-identity snapshots around measurement.
An existing phase directory with those files is accepted; an existing
`result.json` is never overwritten. The report includes `endpoint.channelMask`
under `endpoint.format`, and per-channel values preserve that layout. Only treat
the first two as left/right when channel-mask bits 0 and 1 identify that pair.

Exit zero means measurement completed, not that a scenario passed. Check meter
errors/discontinuities, sufficient active frames, stable endpoint/format, source
freshness, scene/stream identity, and the scenario-specific thresholds. A silent
endpoint can legitimately produce zero packets. Build and measurement records
include binary and input hashes. No CEF-specific telemetry is assumed.
