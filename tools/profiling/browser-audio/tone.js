// Test-only addition to the reserved trusted chrome in a disposable MO2 mod.
// It is never included in the shipped browser view or remote content.
let audio = null;
let oscillator = null;
let reply = null;
osfui.on("osfui-browser-audio.command", async ({ id, action }) => {
  try {
    if (action === "start") {
      if (audio) await audio.close();
      audio = new AudioContext({ sampleRate: 48000 });
      oscillator = audio.createOscillator();
      oscillator.frequency.value = 997;
      const gain = audio.createGain();
      gain.gain.value = 0.03;
      oscillator.connect(gain).connect(audio.destination);
      oscillator.start();
      await audio.resume();
    } else if (action === "stop") {
      if (audio) await audio.close();
      audio = null;
      oscillator = null;
    } else {
      throw new Error("Unsupported audio test command");
    }
    reply = { id, action, ok: true };
  } catch (error) {
    reply = { id, action, ok: false, error: String(error) };
  }
});
// The private chrome observer includes this in its regular heartbeat.
window.osfAudioProbeSnapshot = () => ({
  reply, state: audio?.state ?? "closed",
  time: audio?.currentTime ?? 0, toneHz: oscillator ? 997 : 0
});
