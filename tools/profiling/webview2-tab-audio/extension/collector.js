"use strict";

(() => {
  const defaultSourceUrl = "https://osfui-tab-audio-source.test/source.html";
  const allowedSourceUrls = new Set([
    defaultSourceUrl,
    "https://media.w3.org/2010/05/sintel/trailer.mp4",
  ]);
  const sourceUrl = new URL(location.href).searchParams.get("source") ?? defaultSourceUrl;
  const extension = globalThis.chrome;
  const probe = window.probe = {
    schema: 1,
    sourceUrl,
    collectorUrl: location.href,
    extensionId: extension?.runtime?.id ?? null,
    ready: false,
    state: "initializing",
    mode: "silent",
    attempt: 0,
    updatedAt: new Date().toISOString(),
    capabilities: {
      chrome: typeof extension,
      tabs: typeof extension?.tabs,
      tabsQuery: typeof extension?.tabs?.query,
      tabCapture: typeof extension?.tabCapture,
      getMediaStreamId: typeof extension?.tabCapture?.getMediaStreamId,
      getCapturedTabs: typeof extension?.tabCapture?.getCapturedTabs,
      commands: typeof extension?.commands,
      permissionsGetAll: typeof extension?.permissions?.getAll,
      getUserMedia: typeof navigator.mediaDevices?.getUserMedia,
      audioContext: typeof globalThis.AudioContext,
      pannerNode: typeof globalThis.PannerNode,
      isSecureContext: globalThis.isSecureContext,
    },
    tabs: [],
    permissions: null,
    targetTabId: null,
    tracks: [],
    audio: null,
    errors: [],
    events: [],
  };

  let stream;
  let context;
  let source;
  let analyser;
  let panner;
  let outputGain;
  let sampleBuffer;
  let meterTimer;
  let busy = false;
  let generation = 0;
  let meterReads = 0;
  let largestRms = 0;

  function event(name, detail = {}) {
    probe.updatedAt = new Date().toISOString();
    probe.events.push({ at: probe.updatedAt, name, ...detail });
    if (probe.events.length > 100) probe.events.shift();
  }

  function error(stage, value) {
    const record = {
      at: new Date().toISOString(),
      stage,
      name: value?.name ?? "Error",
      message: value?.message ?? String(value),
      stack: value?.stack ?? null,
      constraint: value?.constraint ?? null,
    };
    probe.errors.push(record);
    probe.lastError = record;
    probe.state = "error";
    event("error", { stage, message: record.message });
  }

  function callbackApi(invoke) {
    return new Promise((resolve, reject) => {
      try {
        invoke(result => {
          const lastError = extension?.runtime?.lastError;
          if (lastError) reject(new Error(lastError.message));
          else resolve(result);
        });
      } catch (failure) {
        reject(failure);
      }
    });
  }

  async function enumerateTabs() {
    if (typeof extension?.tabs?.query !== "function") {
      throw new Error("chrome.tabs.query is unavailable");
    }
    const tabs = await callbackApi(done => extension.tabs.query({}, done));
    probe.tabs = tabs.map(tab => ({
      id: tab.id,
      url: tab.url,
      title: tab.title,
      active: tab.active,
      windowId: tab.windowId,
      audible: tab.audible,
      mutedInfo: tab.mutedInfo,
    }));
    return tabs;
  }

  function updateMeter() {
    if (!context || !analyser || !sampleBuffer) return;
    analyser.getFloatTimeDomainData(sampleBuffer);
    let sumSquares = 0;
    let peak = 0;
    for (const sample of sampleBuffer) {
      sumSquares += sample * sample;
      peak = Math.max(peak, Math.abs(sample));
    }
    const rms = Math.sqrt(sumSquares / sampleBuffer.length);
    largestRms = Math.max(largestRms, rms);
    probe.audio = {
      contextState: context.state,
      sampleRate: context.sampleRate,
      currentTime: context.currentTime,
      baseLatency: context.baseLatency ?? null,
      outputLatency: context.outputLatency ?? null,
      rms,
      peak,
      largestRms,
      meterReads: ++meterReads,
      samplesPerRead: sampleBuffer.length,
      measurement: "pre-panner mono analyser snapshots; overlapping samples possible",
      outputGain: outputGain.gain.value,
      distanceModel: panner.distanceModel,
      refDistance: panner.refDistance,
      rolloffFactor: panner.rolloffFactor,
      pannerPosition: {
        x: panner.positionX.value,
        y: panner.positionY.value,
        z: panner.positionZ.value,
      },
    };
    probe.tracks = stream.getAudioTracks().map(track => ({
      id: track.id,
      label: track.label,
      enabled: track.enabled,
      muted: track.muted,
      readyState: track.readyState,
      settings: track.getSettings(),
    }));
    probe.updatedAt = new Date().toISOString();
  }

  window.setAudioMode = mode => {
    if (!["silent", "center", "left", "right", "far"].includes(mode)) {
      error("setAudioMode", new Error(`Invalid audio mode: ${String(mode)}`));
      return false;
    }
    probe.mode = mode;
    if (context && outputGain && panner) {
      const time = context.currentTime;
      const x = mode === "left" ? -4 : mode === "right" ? 4 : 0;
      panner.positionX.setValueAtTime(x, time);
      panner.positionY.setValueAtTime(0, time);
      panner.positionZ.setValueAtTime(mode === "far" ? -20 : -1, time);
      outputGain.gain.cancelScheduledValues(time);
      outputGain.gain.setValueAtTime(mode === "silent" ? 0 : 1, time);
      updateMeter();
    }
    event("audio-mode", { mode });
    return true;
  };

  async function releaseGraph() {
    if (meterTimer) clearInterval(meterTimer);
    meterTimer = undefined;
    if (outputGain && context) outputGain.gain.value = 0;
    const oldStream = stream;
    const oldContext = context;
    stream = context = source = analyser = panner = outputGain = sampleBuffer = undefined;
    oldStream?.getTracks().forEach(track => track.stop());
    if (oldContext && oldContext.state !== "closed") await oldContext.close();
  }

  async function consumeStreamId(streamId, requestGeneration, mode) {
    if (typeof streamId !== "string" || streamId.length === 0) {
      throw new Error("Capture API returned an empty stream ID");
    }
    probe.state = "opening-stream";
    event("stream-id-received"); // The temporary stream credential is not logged.
    const acquired = await navigator.mediaDevices.getUserMedia({
      audio: {
        mandatory: {
          chromeMediaSource: "tab",
          chromeMediaSourceId: streamId,
        },
      },
      video: false,
    });
    if (requestGeneration !== generation) {
      acquired.getTracks().forEach(track => track.stop());
      return false;
    }
    stream = acquired;
    if (stream.getAudioTracks().length === 0) throw new Error("Capture returned no audio track");
    context = new AudioContext({ latencyHint: "interactive" });
    source = context.createMediaStreamSource(stream);
    analyser = context.createAnalyser();
    analyser.fftSize = 2048;
    analyser.smoothingTimeConstant = 0;
    sampleBuffer = new Float32Array(analyser.fftSize);
    panner = context.createPanner();
    panner.panningModel = "HRTF";
    panner.distanceModel = "inverse";
    panner.refDistance = 1;
    panner.rolloffFactor = 1;
    panner.channelCount = 1;
    panner.channelCountMode = "explicit";
    outputGain = context.createGain();
    outputGain.gain.value = 0;
    source.connect(analyser);
    analyser.connect(panner);
    panner.connect(outputGain);
    outputGain.connect(context.destination);
    meterReads = 0;
    largestRms = 0;
    for (const track of stream.getTracks()) {
      track.addEventListener("ended", () => {
        if (requestGeneration !== generation) return;
        if (outputGain) outputGain.gain.value = 0;
        probe.state = "ended";
        event("track-ended", { id: track.id });
        updateMeter();
      });
      track.addEventListener("mute", () => event("track-muted", { id: track.id }));
      track.addEventListener("unmute", () => event("track-unmuted", { id: track.id }));
    }
    probe.state = "resuming-audio-context";
    updateMeter();
    event("audio-graph-created", { contextState: context.state });
    await context.resume();
    if (requestGeneration !== generation) return false;
    if (!window.setAudioMode(mode)) throw new Error("Invalid requested output mode");
    probe.state = "capturing";
    meterTimer = setInterval(updateMeter, 100);
    updateMeter();
    event("capture-started", { mode });
    return true;
  }

  window.startCapture = async (mode = "silent") => {
    if (busy || stream) {
      event("capture-rejected", { reason: busy ? "request-in-progress" : "already-capturing" });
      return false;
    }
    busy = true;
    const requestGeneration = ++generation;
    probe.attempt++;
    probe.lastError = null;
    probe.state = "enumerating-tabs";
    let stage = "enumerate-tabs";
    try {
      if (!allowedSourceUrls.has(sourceUrl)) {
        stage = "source-url";
        throw new Error("Collector source is not one of the two permitted probe URLs");
      }
      const tabs = await enumerateTabs();
      const candidates = tabs.filter(tab => tab.url === sourceUrl);
      if (candidates.length !== 1) {
        throw new Error(`Expected one exact source tab; found ${candidates.length}`);
      }
      probe.targetTabId = candidates[0].id;
      if (!Number.isInteger(probe.targetTabId)) throw new Error("Source tab has no integer ID");
      if (typeof extension?.tabCapture?.getMediaStreamId !== "function") {
        throw new Error("chrome.tabCapture.getMediaStreamId is unavailable");
      }
      if (requestGeneration !== generation) return false;
      probe.state = "requesting-stream-id";
      stage = "get-media-stream-id";
      const streamId = await callbackApi(done => extension.tabCapture.getMediaStreamId({
        targetTabId: probe.targetTabId,
      }, done));
      if (requestGeneration !== generation) return false;
      stage = "consume-stream-id";
      return await consumeStreamId(streamId, requestGeneration, mode);
    } catch (failure) {
      if (requestGeneration === generation) {
        error(stage, failure);
        await releaseGraph();
      }
      return false;
    } finally {
      busy = false;
    }
  };

  window.stopCapture = async () => {
    ++generation;
    try {
      await releaseGraph();
      probe.state = "stopped";
      probe.mode = "silent";
      event("capture-stopped");
      return true;
    } catch (failure) {
      error("stop-capture", failure);
      return false;
    }
  };

  extension?.runtime?.onMessage?.addListener((message, sender, sendResponse) => {
    if (sender.id !== extension.runtime.id) return false;
    if (message?.kind === "osfui-tab-audio-command-error") {
      error("extension-command", new Error(message.error));
      sendResponse({ accepted: true });
      return false;
    }
    if (message?.kind !== "osfui-tab-audio-command-stream") return false;
    if (!allowedSourceUrls.has(sourceUrl) || message.sourceUrl !== sourceUrl) {
      sendResponse({ accepted: false, error: "Command source does not match this collector's permitted source" });
      return false;
    }
    if (busy || stream) {
      sendResponse({ accepted: false, error: "Collector is busy" });
      return false;
    }
    busy = true;
    const requestGeneration = ++generation;
    probe.attempt++;
    probe.targetTabId = message.targetTabId;
    probe.lastError = null;
    event("extension-command", { targetTabId: message.targetTabId });
    consumeStreamId(message.streamId, requestGeneration, "silent").then(accepted => {
      sendResponse({ accepted });
    }).catch(async failure => {
      error("consume-command-stream-id", failure);
      await releaseGraph();
      sendResponse({ accepted: false, error: failure.message });
    }).finally(() => { busy = false; });
    return true;
  });

  window.addEventListener("error", failure => error("window-error", failure.error ?? failure.message));
  window.addEventListener("unhandledrejection", failure => error("unhandled-rejection", failure.reason));
  probe.ready = true;
  probe.state = "ready";
  event("collector-ready");
  if (!allowedSourceUrls.has(sourceUrl)) {
    error("source-url", new Error("Collector source is not one of the two permitted probe URLs"));
    return;
  }
  enumerateTabs().catch(failure => error("initial-enumerate-tabs", failure));
  if (typeof extension?.permissions?.getAll === "function") {
    callbackApi(done => extension.permissions.getAll(done)).then(permissions => {
      probe.permissions = permissions;
      event("permissions-read");
    }).catch(failure => {
      probe.permissionsError = failure.message;
      event("permissions-diagnostic-failed", { message: failure.message });
    });
  }
})();
