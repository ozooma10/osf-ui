#!/usr/bin/env node
// Analyze saved evidence only. Never launches a browser or changes result.json.
import { readFile, writeFile } from "node:fs/promises";
import { createHash } from "node:crypto";
import path from "node:path";
import { pathToFileURL } from "node:url";
import { analyzeProductionReport } from "./analyze-production.mjs";

const thresholds = Object.freeze({
  audibleRms: 1e-4,
  maximumSuppressedRatio: 0.01,
  minimumDirectionalRatio: 1.3,
  minimumRestoredRatio: 0.8,
  maximumRestoredRatio: 1.2,
  maximumFarRatio: 0.2,
  minimumFarCapturedRatio: 0.5,
  minimumMeasuredSeconds: 1,
  minimumQuietWallSeconds: 0.5,
  minimumToneBlocks: 5,
});
const requiredPhases = [
  "quiet-background",
  "original-browser-audio",
  "capture-without-output",
  "spatial-center",
  "spatial-left",
  "spatial-right",
  "capture-stopped-original-restored",
];
const spatialPhases = ["spatial-center", "spatial-left", "spatial-right"];
const nativeMutePhases = ["native-source-muted-capture-silent", "native-source-muted-capture-center",
  "native-source-muted-capture-stopped", "native-source-muted-capture-restarted",
  "native-source-muted-restarted-capture-stopped"];
const nativeMuteStoppedPhases = new Set([nativeMutePhases[2], nativeMutePhases[4]]);
const finite = value => typeof value === "number" && Number.isFinite(value);
const nonnegative = value => finite(value) && value >= 0;
const positiveInteger = value => Number.isInteger(value) && value > 0;
const ratio = (numerator, denominator) =>
  nonnegative(numerator) && finite(denominator) && denominator > 0
    ? numerator / denominator : null;
const ratioDb = value => finite(value) && value > 0 ? 20 * Math.log10(value) : null;

export function analyzeReport(report, metadata = {}) {
  if (report.productionRoute === true) return analyzeProductionReport(report, metadata);
  const assertions = [];
  const remote = report.remoteVideo === true;
  const metric = remote ? "rms" : "toneRms";
  const channelMetric = remote ? "channelRms" : "channelToneRms";
  const phases = Array.isArray(report.phases) ? report.phases : [];
  const byName = new Map(phases.map(phase => [phase?.name, phase]));
  const get = name => byName.get(name);
  const endpoint = name => get(name)?.endpoint;
  const level = name => endpoint(name)?.[metric];
  const captured = name => get(name)?.collector?.audio?.rms;
  const add = (id, passed, expected, actual = {}, scope = "required") => {
    assertions.push({ id, status: passed ? "pass" : "fail", scope, expected, actual });
  };
  const skip = (id, reason, actual = {}) =>
    assertions.push({ id, status: "skip", scope: "optional", reason, actual });
  const passed = id => assertions.find(assertion => assertion.id === id)?.status === "pass";

  add("report-completed", report.probe === "webview2-extension-tab-audio" &&
    report.result === "capture-completed-needs-measurement-review" &&
    report.exitCode === 0 && !report.error,
  "The probe completed all requested phases without an infrastructure or capture error.",
  { probe: report.probe, result: report.result, exitCode: report.exitCode, error: report.error ?? null });
  add("required-phases-present", requiredPhases.every(name => byName.has(name)),
    "Every required measurement phase is present.",
    { missing: requiredPhases.filter(name => !byName.has(name)), present: [...byName.keys()] });
  add("phase-names-unique", byName.size === phases.length &&
    phases.every(phase => typeof phase?.name === "string" && phase.name.length > 0),
  "Phase names are unique nonempty strings.",
  { names: phases.map(phase => phase?.name ?? null) });
  const requiredIndexes = requiredPhases.map(name => phases.findIndex(phase => phase?.name === name));
  add("phase-order", requiredIndexes.every((index, at) => index >= 0 &&
    (at === 0 || index > requiredIndexes[at - 1])),
  "Required phases occur in their prescribed order.", { indexes: requiredIndexes });
  add("capture-api-succeeded", report.captureAttempt?.state === "capturing" &&
    !report.captureAttempt?.lastError &&
    Array.isArray(report.captureAttempt?.errors) && report.captureAttempt.errors.length === 0,
  "Tab capture reached capturing without a recorded error.",
  { state: report.captureAttempt?.state, errors: report.captureAttempt?.errors ?? null });

  const expectedEndpoint = endpoint("original-browser-audio")?.endpointId;
  const expectedFormat = endpoint("original-browser-audio")?.format;
  let previousGeneration = -1;
  for (const phase of phases) {
    const meter = phase?.endpoint;
    const quiet = phase?.name === "quiet-background" || nativeMuteStoppedPhases.has(phase?.name);
    const channels = meter?.format?.channels;
    const rate = meter?.format?.sampleRate;
    const idleQuiet = quiet && meter?.frames === 0 && meter?.packets === 0;
    const healthyNumbers = ["rms", "peak", "toneRms", "seconds", "wallSeconds"].every(
      key => nonnegative(meter?.[key]));
    const channelValues = ["channelRms", "channelPeak", "channelToneRms"].every(
      key => Array.isArray(meter?.[key]) && meter[key].length === channels &&
        meter[key].every(nonnegative));
    const measuredEnough = quiet
      ? finite(meter?.wallSeconds) && meter.wallSeconds >= thresholds.minimumQuietWallSeconds
      : positiveInteger(rate) && Number.isInteger(meter?.frames) &&
        meter.frames >= rate * thresholds.minimumMeasuredSeconds;
    const toneEnough = remote || quiet ||
      (Number.isInteger(meter?.toneBlocks) && meter.toneBlocks >= thresholds.minimumToneBlocks);
    add(`meter:${phase?.name}`, meter?.ready === true && meter?.running === true &&
      meter?.error === null && meter?.discontinuities === 0 &&
      meter?.timestampErrors === 0 && meter?.invalidSamples === 0 &&
      positiveInteger(rate) && positiveInteger(channels) &&
      Number.isInteger(meter?.frames) && meter.frames >= 0 &&
      healthyNumbers && channelValues && measuredEnough && toneEnough,
    quiet
      ? "A healthy meter observed the quiet interval; zero packets are allowed for an idle endpoint."
      : "A healthy meter recorded at least one second with no discontinuities or invalid samples.",
    {
      frames: meter?.frames, seconds: meter?.seconds, wallSeconds: meter?.wallSeconds,
      idleEndpointAllowed: idleQuiet, channels, rate, toneBlocks: meter?.toneBlocks,
      ready: meter?.ready, running: meter?.running, error: meter?.error ?? null,
      discontinuities: meter?.discontinuities, timestampErrors: meter?.timestampErrors,
      invalidSamples: meter?.invalidSamples,
    });
    add(`endpoint-stable:${phase?.name}`, typeof expectedEndpoint === "string" &&
      expectedEndpoint.length > 0 && meter?.endpointId === expectedEndpoint &&
      meter?.format?.sampleRate === expectedFormat?.sampleRate &&
      meter?.format?.channels === expectedFormat?.channels &&
      meter?.format?.channelMask === expectedFormat?.channelMask,
    "All phases measured the same endpoint and channel layout.",
    { endpointId: meter?.endpointId, format: meter?.format ?? null });
    add(`phase-reset:${phase?.name}`, positiveInteger(meter?.phaseGeneration) &&
      meter.phaseGeneration > previousGeneration,
    "Each phase begins after a distinct acknowledged meter reset.",
    { generation: meter?.phaseGeneration, previousGeneration });
    previousGeneration = meter?.phaseGeneration ?? previousGeneration;
  }

  const original = level("original-browser-audio");
  const silent = level("capture-without-output");
  const background = level("quiet-background");
  const center = level("spatial-center");
  const restored = level("capture-stopped-original-restored");
  add("original-audible", finite(original) && original > thresholds.audibleRms,
    `Original endpoint ${metric} exceeds ${thresholds.audibleRms}.`, { metric, value: original });
  add("background-low", nonnegative(background) && finite(original) &&
    original > thresholds.audibleRms && background / original < thresholds.maximumSuppressedRatio,
  "Background level is below one percent of the original source level.",
  { metric, background, original, ratio: ratio(background, original) });
  add("original-output-suppressed", nonnegative(silent) && finite(original) &&
    original > thresholds.audibleRms && silent / original < thresholds.maximumSuppressedRatio,
  "With replacement output disabled, endpoint level is below one percent of original.",
  { metric, silent, original, ratio: ratio(silent, original), ratioDb: ratioDb(ratio(silent, original)),
    evidence: remote ? "nonstationary-video-observation" : "controlled-tone" });
  add("replacement-output-disabled", get("capture-without-output")?.collector?.mode === "silent" &&
    get("capture-without-output")?.collector?.audio?.outputGain === 0,
  "The collector explicitly disabled its replacement output during the suppression measurement.",
  { mode: get("capture-without-output")?.collector?.mode,
    outputGain: get("capture-without-output")?.collector?.audio?.outputGain });

  const capturePhases = ["capture-without-output", ...spatialPhases];
  if (byName.has("spatial-far")) capturePhases.push("spatial-far");
  capturePhases.sort((a, b) => phases.findIndex(phase => phase?.name === a) -
    phases.findIndex(phase => phase?.name === b));
  const firstTrack = get("capture-without-output")?.collector?.tracks?.find(
    track => track?.readyState === "live");
  const targetTabId = get("capture-without-output")?.collector?.targetTabId;
  let previousReads = report.captureAttempt?.audio?.meterReads ?? -1;
  let previousTime = report.captureAttempt?.audio?.currentTime ?? -1;
  for (const name of capturePhases) {
    const collector = get(name)?.collector;
    const audio = collector?.audio;
    const liveTrack = Array.isArray(collector?.tracks) && collector.tracks.some(
      track => track?.enabled === true && track?.muted === false &&
        track?.readyState === "live" && track?.id === firstTrack?.id);
    add(`capture-healthy:${name}`, collector?.state === "capturing" &&
      audio?.contextState === "running" && finite(audio?.rms) &&
      audio.rms > thresholds.audibleRms && liveTrack &&
      Number.isInteger(collector?.targetTabId) && collector.targetTabId === targetTabId &&
      Number.isInteger(audio?.meterReads) && audio.meterReads > previousReads &&
      finite(audio?.currentTime) && audio.currentTime > previousTime &&
      !collector?.lastError && Array.isArray(collector?.errors) && collector.errors.length === 0,
    "The same live source remains captured with fresh analyser samples above the absolute RMS floor.",
    { rms: audio?.rms, state: collector?.state, contextState: audio?.contextState,
      meterReads: audio?.meterReads, previousReads, currentTime: audio?.currentTime,
      previousTime, liveTrack, targetTabId: collector?.targetTabId,
      errors: collector?.errors ?? null });
    previousReads = audio?.meterReads ?? previousReads;
    previousTime = audio?.currentTime ?? previousTime;
    if (name !== "capture-without-output") {
      const expectedMode = name.slice("spatial-".length);
      add(`replacement-enabled:${name}`, collector?.mode === expectedMode &&
        finite(audio?.outputGain) && audio.outputGain > 0,
      "The requested spatial replacement mode is active with a positive output gain.",
      { expectedMode, mode: collector?.mode, outputGain: audio?.outputGain,
        position: audio?.pannerPosition ?? null });
    }
  }
  add("center-audible", finite(center) && center > thresholds.audibleRms,
    "Replacement output at the center position is audible.", { metric, value: center });

  // Only the front-left/front-right pair is judged. The meter preserves the
  // native layout, so an unidentified or mono layout cannot establish panning.
  const mask = expectedFormat?.channelMask;
  const knownStereoPair = Number.isInteger(mask) && (mask & 3) === 3 &&
    expectedFormat?.channels >= 2;
  add("left-right-layout-known", knownStereoPair,
    "The measured channel mask identifies front-left and front-right as its first two channels.",
    { channels: expectedFormat?.channels, channelMask: mask });
  for (const [name, favoredIndex, otherIndex] of [
    ["spatial-left", 0, 1], ["spatial-right", 1, 0],
  ]) {
    const values = endpoint(name)?.[channelMetric];
    const favored = values?.[favoredIndex], other = values?.[otherIndex];
    add(`direction:${name}`, knownStereoPair && finite(favored) && nonnegative(other) &&
      favored > thresholds.audibleRms && favored > thresholds.minimumDirectionalRatio * other,
    "The requested side exceeds the opposite side by a ratio greater than 1.3.",
    { metric: channelMetric, favored, other, ratio: ratio(favored, other),
      ratioDb: ratioDb(ratio(favored, other)), zeroOppositeChannel: other === 0 });
  }

  add("capture-stopped", get("capture-stopped-original-restored")?.collector?.state === "stopped",
    "The collector reports capture stopped before the restoration measurement.",
    { state: get("capture-stopped-original-restored")?.collector?.state,
      note: "Stopped collector audio/track snapshots may be stale and are not used as proof." });
  if (remote) {
    skip("restored-amplitude", "A changing video soundtrack cannot establish restoration by comparison with an earlier amplitude.",
      { original, restored, observedRatio: ratio(restored, original) });
  } else {
    const restoredRatio = ratio(restored, original);
    add("restored-amplitude", finite(restoredRatio) &&
      restoredRatio >= thresholds.minimumRestoredRatio &&
      restoredRatio <= thresholds.maximumRestoredRatio,
    "Stopping capture restores the constant tone to 80–120 percent of its original level.",
    { original, restored, ratio: restoredRatio });
  }

  if (!byName.has("spatial-far")) {
    skip("distance-falloff", "This run did not include a spatial-far measurement.");
  } else if (remote) {
    skip("distance-falloff", "Changing video content makes a cross-phase far/center amplitude comparison inconclusive.",
      { center, far: level("spatial-far"), observedRatio: ratio(level("spatial-far"), center),
        capturedRms: captured("spatial-far") });
  } else {
    const farRatio = ratio(level("spatial-far"), center);
    const retainedRatio = ratio(captured("spatial-far"), captured("spatial-center"));
    add("distance-falloff", passed("capture-healthy:spatial-far") &&
      finite(farRatio) && farRatio < thresholds.maximumFarRatio &&
      finite(retainedRatio) && retainedRatio > thresholds.minimumFarCapturedRatio,
    "Far output is below 20 percent of center while the captured source retains more than half its center RMS.",
    { center, far: level("spatial-far"), ratio: farRatio,
      capturedCenterRms: captured("spatial-center"), capturedFarRms: captured("spatial-far"),
      retainedCapturedRatio: retainedRatio });
  }

  const nativeMuteChecks = [];
  if (report.nativeSourceMute === true) {
    const addMute = (id, condition, expected, actual) => {
      nativeMuteChecks.push(id);
      add(id, condition, expected, actual);
    };
    addMute("native-mute-phases-present", nativeMutePhases.every(name => byName.has(name)),
      "Every requested native-mute guard measurement is present.",
      { missing: nativeMutePhases.filter(name => !byName.has(name)) });
    for (const name of nativeMutePhases) {
      const phase = get(name), collector = phase?.collector, audio = collector?.audio;
      addMute(`native-mute-readback:${name}`, phase?.source?.nativeMuted === true,
        "The source CoreWebView2_8 native mute reads back true throughout the guard test.",
        { nativeMuted: phase?.source?.nativeMuted });
      if (nativeMuteStoppedPhases.has(name)) {
        const suppressedRatio = ratio(level(name), original);
        addMute(`native-mute-stopped-silent:${name}`, collector?.state === "stopped" &&
          finite(suppressedRatio) && suppressedRatio < thresholds.maximumSuppressedRatio,
          "After capture stops, native source mute keeps endpoint output below one percent of original.",
          { state: collector?.state, level: level(name), original, ratio: suppressedRatio,
            note: "Stopped collector analyser values are stale and intentionally ignored." });
        continue;
      }
      const restarted = name === "native-source-muted-capture-restarted";
      const prior = restarted ? report.mutedCaptureAttempt : get("spatial-far")?.collector ?? get("spatial-right")?.collector;
      const liveTrack = Array.isArray(collector?.tracks) && collector.tracks.some(track =>
        track?.enabled === true && track?.muted === false && track?.readyState === "live" &&
        prior?.tracks?.some(previous => previous.id === track.id));
      addMute(`native-mute-capture-survives:${name}`, collector?.state === "capturing" &&
        audio?.contextState === "running" && finite(audio?.rms) && audio.rms > thresholds.audibleRms &&
        liveTrack && audio.meterReads > prior?.audio?.meterReads &&
        audio.currentTime > prior?.audio?.currentTime && collector?.targetTabId === targetTabId &&
        !collector?.lastError && Array.isArray(collector?.errors) && collector.errors.length === 0,
        "A fresh, live tab capture produces nonzero PCM while the source stays natively muted.",
        { capturedRms: audio?.rms, state: collector?.state, liveTrack,
          meterReads: audio?.meterReads, previousReads: prior?.audio?.meterReads });
      const silentMode = name === "native-source-muted-capture-silent";
      const outputRatio = ratio(level(name), silentMode ? original : center);
      addMute(`native-mute-replacement:${name}`, collector?.mode === (silentMode ? "silent" : "center") &&
        audio?.outputGain === (silentMode ? 0 : 1) && finite(outputRatio) &&
        (silentMode ? outputRatio < thresholds.maximumSuppressedRatio
          : remote ? level(name) > thresholds.audibleRms
          : outputRatio >= thresholds.minimumRestoredRatio && outputRatio <= thresholds.maximumRestoredRatio),
        silentMode ? "Replacement disabled with native mute is silent."
          : "Replacement center output remains audible while source native mute is enabled.",
        { mode: collector?.mode, gain: audio?.outputGain, level: level(name), ratio: outputRatio });
    }
    addMute("native-mute-restored", typeof report.originalSourceMuted === "boolean" &&
      get("capture-stopped-original-restored")?.source?.nativeMuted === report.originalSourceMuted &&
      report.finalSource?.nativeMuted === report.originalSourceMuted,
      "The probe restored its initial source native mute state.",
      { initial: report.originalSourceMuted, restored: get("capture-stopped-original-restored")?.source?.nativeMuted,
        final: report.finalSource?.nativeMuted });
  }

  const failures = assertions.filter(assertion => assertion.status === "fail");
  const skips = assertions.filter(assertion => assertion.status === "skip");
  const meterHealthy = assertions.filter(assertion =>
    assertion.id.startsWith("meter:") || assertion.id.startsWith("endpoint-stable:") ||
    assertion.id.startsWith("phase-reset:")).every(assertion => assertion.status === "pass");
  const prerequisites = ["report-completed", "required-phases-present", "phase-names-unique",
    "phase-order", "capture-api-succeeded", "original-audible", "background-low"].every(passed) &&
    meterHealthy;
  const claims = {
    captureSurvivesOriginalSuppression: prerequisites &&
      ["original-output-suppressed", "replacement-output-disabled",
        "capture-healthy:capture-without-output"].every(passed),
    leftRightPositioning: prerequisites && ["center-audible", "left-right-layout-known",
      "direction:spatial-left", "direction:spatial-right",
      ...spatialPhases.flatMap(name => [`capture-healthy:${name}`, `replacement-enabled:${name}`])].every(passed),
    originalOutputRestored: prerequisites && passed("capture-stopped") && passed("restored-amplitude"),
    distanceFalloff: prerequisites && passed("distance-falloff") && passed("replacement-enabled:spatial-far"),
    nativeMuteGuard: report.nativeSourceMute === true && prerequisites && nativeMuteChecks.every(passed),
  };
  return {
    schema: 1,
    analyzer: "webview2-tab-audio-measurements",
    ...metadata,
    mode: remote ? "remote-video" : "constant-tone",
    evidenceScope: remote ? "nonstationary-video-observations" : "controlled-tone",
    status: failures.length ? "fail" : skips.length ? "pass-with-skips" : "pass",
    passed: failures.length === 0,
    counts: { pass: assertions.length - failures.length - skips.length,
      fail: failures.length, skip: skips.length },
    thresholds,
    claims,
    limits: [
      "Endpoint measurements are aggregate. Concurrent unrelated sound can invalidate attribution.",
      "The source analyser is a pre-panner mono snapshot; it is not a continuous PCM recording.",
      "Original endpoint and source analyser amplitudes are not directly compared; output processing may differ.",
      ...(remote ? [
        "Video amplitude changes between phases. Suppression and directional results are observations, not controlled-tone proof.",
        "Restoration and distance claims require a stationary source or matched content intervals.",
      ] : []),
      "This report validates a standalone browser experiment, not game integration or subjective listening quality.",
    ],
    assertions,
  };
}

async function main() {
  const inputs = process.argv.slice(2);
  if (!inputs.length || inputs.some(argument => argument === "--help" || argument === "-h")) {
    console.log("Usage: node analyze.mjs <result.json> [<another/result.json> ...]");
    return inputs.length ? 0 : 1;
  }
  let exitCode = 0;
  const outputs = new Set();
  for (const input of inputs) {
    const sourceReport = path.resolve(input);
    const outputPath = path.join(path.dirname(sourceReport), "assertions.json");
    try {
      if (sourceReport.toLowerCase() === outputPath.toLowerCase())
        throw new Error("Refusing to overwrite an input named assertions.json");
      if (outputs.has(outputPath.toLowerCase()))
        throw new Error("Multiple inputs in one directory would overwrite the same assertions.json");
      outputs.add(outputPath.toLowerCase());
      const bytes = await readFile(sourceReport);
      const report = JSON.parse(bytes.toString("utf8").replace(/^\uFEFF/, ""));
      if (!report || typeof report !== "object" || Array.isArray(report))
        throw new Error("Input report must be a JSON object");
      const assessment = analyzeReport(report, {
        sourceReport,
        sourceSha256: createHash("sha256").update(bytes).digest("hex"),
        analyzedAt: new Date().toISOString(),
      });
      await writeFile(outputPath, `${JSON.stringify(assessment, null, 2)}\n`, "utf8");
      const summary = {
        report: sourceReport, assertions: outputPath, status: assessment.status,
        counts: assessment.counts, claims: assessment.claims,
        failed: assessment.assertions.filter(assertion => assertion.status === "fail").map(assertion => assertion.id),
      };
      console.log(JSON.stringify(summary));
      if (!assessment.passed && exitCode === 0) exitCode = 2;
    } catch (error) {
      console.error(JSON.stringify({ report: sourceReport, status: "analysis-error", error: error.message }));
      exitCode = 1;
    }
  }
  return exitCode;
}

if (process.argv[1] && import.meta.url === pathToFileURL(path.resolve(process.argv[1])).href)
  process.exitCode = await main();
