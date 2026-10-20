(() => {
  let context;
  let oscillator;
  let video;
  window.sourceProbe = {
    state: "idle",
    prepareVideo() {
      video = document.createElement("video");
      video.src = "https://media.w3.org/2010/05/sintel/trailer.mp4";
      video.volume = 0.05;
      video.preload = "auto";
      video.controls = true;
      document.body.appendChild(video);
      video.addEventListener("error", () => { this.state = "video-error"; });
    },
    snapshot() {
      return {state: this.state, video: video ? {
        currentTime: video.currentTime, duration: video.duration,
        paused: video.paused, ended: video.ended, readyState: video.readyState,
        volume: video.volume, error: video.error?.message ?? null
      } : null};
    },
    async start() {
      if (video) {
        try { await video.play(); this.state = "playing-video"; }
        catch (error) { this.state = String(error); }
        return;
      }
      if (context) return;
      context = new AudioContext({sampleRate: 48000});
      oscillator = context.createOscillator();
      oscillator.frequency.value = 997;
      const volume = context.createGain();
      volume.gain.value = 0.02;
      oscillator.connect(volume).connect(context.destination);
      oscillator.start();
      await context.resume();
      this.state = context.state;
    },
    async stop() {
      if (video) video.pause();
      if (context) await context.close();
      context = oscillator = null;
      this.state = "closed";
    }
  };
})();
