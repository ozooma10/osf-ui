"use strict";

(() => {
  const MAX_EMITTERS = 32;
  const MAX_SCENE_MS = 1000;
  const extension = globalThis.chrome;
  let generation = 0;
  let busy = false;
  let terminal = false;
  let stream;
  let context;
  let source;
  let mono;
  let output;
  let scene;
  let clockTimer;
  const emitters = new Map();
  const state = { state: "ready", sourceUrl: "", targetTabId: null, captured: false,
    enabled: false, muted: true, gain: 0, emitterCount: 0, deadlineUtcMs: 0,
    normalization: 1, reason: "", error: "" };

  function silence() {
    state.enabled = false;
    if (!context || !output) return;
    output.gain.cancelScheduledValues(context.currentTime);
    output.gain.setValueAtTime(0, context.currentTime);
  }

  function notify(next, reason = "", error = "") {
    const changed = state.state !== next || state.reason !== reason || state.error !== error;
    state.state = next; state.reason = reason; state.error = error;
    if (!changed && next !== "ready") return;
    try {
      extension.webview.postMessage(JSON.stringify({ kind: "osfui.audio", state: next,
        reason, error, captured: state.captured }));
    } catch {
      terminal = true;
      silence();
      state.state = "failed"; state.reason = "native-channel";
      state.error = "Audio collector cannot report to its native owner";
    }
  }

  function fail(reason, failure) {
    terminal = true;
    silence();
    notify("failed", reason, failure?.message ?? String(failure));
    // Stopping tracks restores the source's original output. Only the native
    // owner may call stop(), after it has muted the source controller.
  }

  function callbackApi(invoke) {
    return new Promise((resolve, reject) => {
      try {
        invoke(value => {
          const failure = extension.runtime.lastError;
          if (failure) reject(new Error(failure.message));
          else resolve(value);
        });
      } catch (failure) { reject(failure); }
    });
  }

  const vector = value => Array.isArray(value) && value.length === 3 &&
    value.every(component => typeof component === "number" && Number.isFinite(component));
  const dot = (a, b) => a[0] * b[0] + a[1] * b[1] + a[2] * b[2];
  const webVector = value => [value[0], value[2], -value[1]];
  const distanceGain = distance => Math.max(0, 1 - (Math.max(1, distance) - 1) / 19);

  function validate(value) {
    if (!value || !vector(value.position) || !vector(value.front) || !vector(value.up) ||
        Math.abs(dot(value.front, value.front) - 1) > 0.02 ||
        Math.abs(dot(value.up, value.up) - 1) > 0.02 ||
        Math.abs(dot(value.front, value.up)) > 0.02 ||
        typeof value.gain !== "number" || !Number.isFinite(value.gain) || value.gain < 0 || value.gain > 1 ||
        typeof value.muted !== "boolean" || !Number.isFinite(value.deadlineUtcMs) ||
        !Array.isArray(value.emitters) || value.emitters.length > MAX_EMITTERS) return false;
    const ids = new Set();
    for (const emitter of value.emitters) {
      if (!emitter || !Number.isInteger(emitter.id) || emitter.id < 1 || emitter.id > 0xffffffff ||
          ids.has(emitter.id) || !vector(emitter.position)) return false;
      ids.add(emitter.id);
    }
    return true;
  }

  function setVector(target, prefix, value, time) {
    const converted = webVector(value);
    for (let index = 0; index < 3; ++index) {
      target[prefix + "XYZ"[index]].setValueAtTime(converted[index], time);
    }
  }

  function applyScene() {
    if (terminal || !context || !output || !scene || !state.captured) return false;
    const remaining = scene.deadlineUtcMs - Date.now();
    if (!(remaining > 0 && remaining <= MAX_SCENE_MS)) {
      silence();
      notify("silent", "scene-expired");
      return false;
    }
    const time = context.currentTime;
    setVector(context.listener, "position", scene.position, time);
    setVector(context.listener, "forward", scene.front, time);
    setVector(context.listener, "up", scene.up, time);
    const desired = new Set(scene.emitters.map(item => item.id));
    for (const [id, nodes] of emitters) {
      if (desired.has(id)) continue;
      mono.disconnect(nodes.panner);
      nodes.panner.disconnect(); nodes.gain.disconnect();
      emitters.delete(id);
    }
    let totalAttenuation = 0;
    for (const emitter of scene.emitters) {
      let nodes = emitters.get(emitter.id);
      if (!nodes) {
        const panner = context.createPanner();
        panner.panningModel = "HRTF";
        panner.distanceModel = "linear";
        panner.refDistance = 1; panner.maxDistance = 20; panner.rolloffFactor = 1;
        panner.channelCount = 1; panner.channelCountMode = "explicit";
        const gain = context.createGain();
        gain.gain.value = 0;
        mono.connect(panner); panner.connect(gain); gain.connect(output);
        nodes = { panner, gain }; emitters.set(emitter.id, nodes);
      }
      setVector(nodes.panner, "position", emitter.position, time);
      totalAttenuation += distanceGain(Math.hypot(...emitter.position.map((v, i) => v - scene.position[i])));
    }
    const normalization = Math.max(1, totalAttenuation);
    for (const nodes of emitters.values()) nodes.gain.gain.setValueAtTime(1 / normalization, time);
    state.normalization = normalization;
    state.emitterCount = scene.emitters.length;
    state.enabled = !scene.muted && scene.gain > 0 && totalAttenuation > 0;
    output.gain.cancelScheduledValues(time);
    output.gain.setValueAtTime(state.enabled ? scene.gain : 0, time);
    // Audio-thread automation expires output even if the renderer JS thread is
    // suspended. Native independently retains the original QPC deadline.
    output.gain.setValueAtTime(0, time + remaining / 1000);
    notify(state.enabled ? "capturing" : "silent", state.enabled ? "" : "scene-silent");
    return true;
  }

  function setScene(value) {
    if (terminal) return false;
    if (!validate(value)) {
      scene = undefined;
      silence(); notify("silent", "invalid-scene"); return false;
    }
    const remaining = value.deadlineUtcMs - Date.now();
    if (!(remaining > 0 && remaining <= MAX_SCENE_MS)) {
      scene = undefined;
      silence(); notify("silent", "scene-expired"); return false;
    }
    // Own the scene snapshot; native calls cannot mutate an accepted update.
    scene = { position: [...value.position], front: [...value.front], up: [...value.up],
      gain: value.gain, muted: value.muted, deadlineUtcMs: value.deadlineUtcMs,
      emitters: value.emitters.map(item => ({ id: item.id, position: [...item.position] })) };
    state.gain = scene.gain; state.muted = scene.muted; state.deadlineUtcMs = scene.deadlineUtcMs;
    try { return context ? applyScene() : true; }
    catch (failure) { fail("audio-scene", failure); return false; }
  }

  async function configure(options) {
    if (busy || stream || terminal) return false;
    busy = true;
    const current = ++generation;
    let stage = "source-url";
    try {
      if (!options || typeof options.sourceUrl !== "string") throw new Error("Missing audio source URL");
      const url = new URL(options.sourceUrl);
      if (!["http:", "https:"].includes(url.protocol) || url.username || url.password)
        throw new Error("Invalid audio source URL");
      state.sourceUrl = options.sourceUrl;
      stage = "find-source";
      const tabs = await callbackApi(done => extension.tabs.query({}, done));
      if (current !== generation) return false;
      const candidates = tabs.filter(tab => tab.url === options.sourceUrl);
      if (candidates.length !== 1 || !Number.isInteger(candidates[0].id))
        throw new Error("Audio source must match exactly one browser tab");
      state.targetTabId = candidates[0].id;
      stage = "capture-permission";
      const id = await callbackApi(done => extension.tabCapture.getMediaStreamId({
        targetTabId: state.targetTabId,
      }, done));
      if (current !== generation) return false;
      if (typeof id !== "string" || !id) throw new Error("Empty audio stream ID");
      stage = "capture-stream";
      const acquired = await navigator.mediaDevices.getUserMedia({ audio: {
        mandatory: { chromeMediaSource: "tab", chromeMediaSourceId: id },
      }, video: false });
      if (current !== generation) {
        acquired.getTracks().forEach(track => track.stop());
        return false;
      }
      stream = acquired;
      const tracks = stream.getAudioTracks();
      if (tracks.length !== 1) throw new Error("Expected one captured audio track");
      tracks[0].addEventListener("ended", () => {
        if (current === generation) fail("capture-ended", new Error("Browser audio capture ended"));
      });
      stage = "audio-context";
      context = new AudioContext({ latencyHint: "interactive" });
      source = context.createMediaStreamSource(stream);
      mono = context.createGain(); mono.channelCount = 1; mono.channelCountMode = "explicit";
      mono.channelInterpretation = "speakers";
      output = context.createGain(); output.gain.value = 0;
      source.connect(mono); output.connect(context.destination);
      context.addEventListener("statechange", () => {
        if (current === generation && state.captured && context?.state !== "running")
          fail("audio-context", new Error("Browser audio context stopped running"));
      });
      await context.resume();
      if (current !== generation) return false;
      if (terminal) return false;
      if (context.state !== "running") throw new Error("Audio context did not resume");
      state.captured = true;
      notify("capturing");
      if (terminal) return false;
      if (scene) applyScene();
      clockTimer = setInterval(() => {
        if (!scene || terminal) return;
        const remaining = scene.deadlineUtcMs - Date.now();
        if (!(remaining > 0 && remaining <= MAX_SCENE_MS)) {
          silence(); notify("silent", "scene-expired");
        }
      }, 100);
      return true;
    } catch (failure) {
      if (current === generation) fail(stage, failure);
      return false;
    } finally { busy = false; }
  }

  async function stop() {
    ++generation;
    silence();
    if (clockTimer) clearInterval(clockTimer);
    clockTimer = undefined; scene = undefined;
    const previousStream = stream, previousContext = context;
    stream = context = source = mono = output = undefined;
    emitters.clear();
    state.captured = false; state.emitterCount = 0; state.deadlineUtcMs = 0;
    try {
      previousStream?.getTracks().forEach(track => track.stop());
      if (previousContext && previousContext.state !== "closed") await previousContext.close();
      terminal = false;
      notify("stopped");
      return true;
    } catch (failure) { fail("audio-stop", failure); return false; }
  }

  window.osfAudio = Object.freeze({ configure, setScene, stop,
    snapshot: () => ({ ...state, contextState: context?.state ?? "none", sampleRate: context?.sampleRate ?? 0 }) });
  window.addEventListener("error", event => fail("collector-script", event.error ?? event.message));
  window.addEventListener("unhandledrejection", event => fail("collector-promise", event.reason));
  notify("ready");
})();
