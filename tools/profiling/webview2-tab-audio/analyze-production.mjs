// Saved evidence from the standalone harness linked to shipping SpatialAudio.
export function analyzeProductionReport(report, metadata = {}) {
  const required = ["quiet-background", "production-idle-about-blank-silent", "production-startup-silent", "production-center",
    "production-left", "production-right", "production-far", "production-logical-muted",
    "production-empty-scene", "production-expired-scene", "production-scene-recovered", "production-navigation-retained",
    "production-failed-guard-silent", "production-route-closed-silent", "production-navigation-recreated"];
  const quiet = new Set(["quiet-background", "production-idle-about-blank-silent", "production-startup-silent", "production-logical-muted",
    "production-empty-scene", "production-expired-scene", "production-failed-guard-silent",
    "production-route-closed-silent"]);
  const active = required.filter(name => !quiet.has(name));
  const phases = Array.isArray(report.phases) ? report.phases : [];
  const lookup = new Map(phases.map(phase => [phase?.name, phase]));
  const phase = name => lookup.get(name);
  const finite = value => typeof value === "number" && Number.isFinite(value);
  const nonnegative = value => finite(value) && value >= 0;
  const remote = report.remoteVideo === true;
  const metric = remote ? "rms" : "toneRms";
  const channelMetric = remote ? "channelRms" : "channelToneRms";
  const level = name => phase(name)?.endpoint?.[metric];
  const center = level("production-center");
  const ratio = value => nonnegative(value) && finite(center) && center > 0 ? value / center : null;
  const assertions = [];
  const add = (id, condition, expected, actual = {}) =>
    assertions.push({id, status: condition ? "pass" : "fail", scope: "required", expected, actual});
  const skip = (id, reason, actual = {}) =>
    assertions.push({id, status: "skip", scope: "optional", reason, actual});
  const passed = id => assertions.find(item => item.id === id)?.status === "pass";
  add("report-completed", report.probe === "webview2-production-spatial-audio" &&
    report.productionRoute === true && report.result === "production-route-completed-needs-measurement-review" &&
    report.exitCode === 0 && !report.error,
    "The shipping SpatialAudio harness completed without an unexpected failure.",
    {result:report.result, exitCode:report.exitCode, error:report.error ?? null});
  const indexes = required.map(name => phases.findIndex(item => item?.name === name));
  add("phase-sequence", phases.length === required.length && lookup.size === phases.length &&
    indexes.every((index, at) => index === at),
    "All production-route measurement phases are present, unique, and ordered.",
    {names:phases.map(item => item?.name)});
  add("center-audible", finite(center) && center > 1e-4,
    "The production center route produces an audible test signal.", {metric,center});
  const expectedEndpoint = phase("production-center")?.endpoint;
  let previousGeneration = -1;
  for (const item of phases) {
    const name = item?.name, meter = item?.endpoint, state = item?.route;
    const silent = quiet.has(name), channels = meter?.format?.channels, rate = meter?.format?.sampleRate;
    const enough = silent ? meter?.wallSeconds >= 0.5 :
      Number.isInteger(meter?.frames) && meter.frames >= rate;
    add(`meter:${name}`, meter?.ready === true && meter?.running === true && meter?.error === null &&
      meter?.discontinuities === 0 && meter?.timestampErrors === 0 && meter?.invalidSamples === 0 &&
      Number.isInteger(rate) && rate > 0 && Number.isInteger(channels) && channels > 0 &&
      Number.isInteger(meter?.frames) && meter.frames >= 0 && enough &&
      (silent || remote || meter?.toneBlocks >= 5) &&
      ["rms","toneRms","peak","wallSeconds","seconds"].every(key => nonnegative(meter?.[key])) &&
      ["channelRms","channelToneRms","channelPeak"].every(key =>
        Array.isArray(meter?.[key]) && meter[key].length === channels && meter[key].every(nonnegative)),
      "A healthy endpoint meter observes each phase; silent phases may have zero packets when the endpoint sleeps.",
      {frames:meter?.frames, wallSeconds:meter?.wallSeconds, toneBlocks:meter?.toneBlocks,
        error:meter?.error, discontinuities:meter?.discontinuities});
    add(`same-endpoint:${name}`, typeof meter?.endpointId === "string" && meter.endpointId.length > 0 &&
      meter.endpointId === expectedEndpoint?.endpointId && channels === expectedEndpoint?.format?.channels &&
      rate === expectedEndpoint?.format?.sampleRate && meter?.format?.channelMask === expectedEndpoint?.format?.channelMask &&
      Number.isInteger(meter?.phaseGeneration) && meter.phaseGeneration > previousGeneration,
      "Each phase uses a new acknowledged reset on the same endpoint and channel layout.",
      {endpointId:meter?.endpointId, phaseGeneration:meter?.phaseGeneration});
    previousGeneration = meter?.phaseGeneration ?? previousGeneration;
    add(`source-guard:${name}`, item?.source?.nativeMuted === true,
      "CoreWebView2_8 source native mute remains true, including when capture fails or closes.",
      {nativeMuted:item?.source?.nativeMuted});
    if (name !== "quiet-background" && name !== "production-idle-about-blank-silent") add(`source-playing:${name}`,
      item?.source?.state === (remote ? "playing-video" : "running"),
      "The source is still playing while the measured routing behavior is observed.",
      {state:item?.source?.state});
    if (silent) {
      const value = ratio(level(name));
      add(`silent:${name}`, finite(value) && value < 0.01,
        "Endpoint output is below one percent of the audible center reference.",
        {metric,level:level(name),center,ratio:value});
    } else {
      add(`route-active:${name}`, state?.state === "capturing" && state?.fresh === true &&
        state?.muted === false && state?.error === "" && item?.refreshScene === true,
        "The production manager reports active capture with a fresh scene and no logical mute or error.",
        {route:state});
      add(`audible:${name}`, finite(level(name)) && level(name) > 1e-4,
        "The replacement output is audible despite the source's native mute.", {metric,level:level(name)});
    }
  }
  const mask = expectedEndpoint?.format?.channelMask;
  const stereo = Number.isInteger(mask) && (mask & 3) === 3 && expectedEndpoint?.format?.channels >= 2;
  add("stereo-layout", stereo, "Front-left and front-right channels are identified.", {channelMask:mask});
  for (const [name, favored, other] of [["production-left",0,1],["production-right",1,0]]) {
    const values = phase(name)?.endpoint?.[channelMetric];
    add(`direction:${name}`, stereo && finite(values?.[favored]) && nonnegative(values?.[other]) &&
      values[favored] > 1e-4 && values[favored] > values[other] * 1.3,
      "The requested side exceeds the opposite side by more than 1.3 times.",
      {channelMetric,channels:values,ratio:values?.[other] > 0 ? values[favored]/values[other] : null});
  }
  add("logical-mute", phase("production-logical-muted")?.route?.muted === true &&
    phase("production-logical-muted")?.route?.fresh === true &&
    phase("production-logical-muted")?.route?.state === "silent" &&
    phase("production-logical-muted")?.scene?.emitters?.length === 1,
    "Logical mute silences an otherwise fresh nonempty scene.");
  add("about-blank-waits-without-failure", report.aboutBlankWaitWallSeconds > 15 &&
    phase("production-idle-about-blank-silent")?.source?.state === "about-blank" &&
    phase("production-idle-about-blank-silent")?.route?.state === "starting" &&
    phase("production-idle-about-blank-silent")?.route?.error === "",
    "The manager waits on about:blank for more than fifteen seconds without timing out.",
    {seconds:report.aboutBlankWaitWallSeconds, route:phase("production-idle-about-blank-silent")?.route});
  add("empty-scene", phase("production-empty-scene")?.scene?.emitters?.length === 0 &&
    phase("production-empty-scene")?.route?.fresh === true &&
    phase("production-empty-scene")?.route?.muted === false && phase("production-empty-scene")?.route?.state === "silent",
    "A fresh empty scene removes the emitter output without relying on logical mute.");
  add("expired-scene", phase("production-expired-scene")?.route?.fresh === false &&
    phase("production-expired-scene")?.route?.state === "silent" &&
    phase("production-expired-scene")?.refreshScene === false &&
    phase("production-expired-scene")?.scene?.emitters?.length === 1,
    "A previously nonempty scene becomes silent after its deadline without further refreshes.");
  add("failure-injected", phase("production-failed-guard-silent")?.route?.state === "failed" &&
    phase("production-failed-guard-silent")?.route?.error === "probe-forced-collector-failure",
    "The real manager's explicit failure path ran before the fail-closed measurement.");
  add("route-destroyed", phase("production-route-closed-silent")?.route?.state === "closed",
    "The route was destroyed before measuring continuing source silence.");
  add("route-recreated", phase("production-navigation-recreated")?.routeGeneration === 2 &&
    phase("production-center")?.routeGeneration === 1,
    "Navigation was followed by a second manager/controller capture generation.");
  add("route-retained", phase("production-navigation-retained")?.routeGeneration === 1,
    "The first route survives source navigation without being replaced.");
  if (remote) {
    skip("distance-falloff", "Nonstationary video does not establish distance attenuation against an earlier interval.");
    skip("center-restoration", "Nonstationary video does not establish amplitude restoration against an earlier interval.");
  } else {
    const farRatio = ratio(level("production-far"));
    add("distance-falloff", finite(farRatio) && farRatio < 0.2,
      "The distant emitter is below twenty percent of center level.", {ratio:farRatio});
    for (const name of ["production-scene-recovered","production-navigation-retained","production-navigation-recreated"]) {
      const value = ratio(level(name));
      add(`center-restoration:${name}`, finite(value) && value >= 0.8 && value <= 1.2,
        "Center output recovers to 80–120 percent of the initial center level.", {ratio:value});
    }
  }
  add("final-source-stopped", report.finalSource?.state === "closed" && report.finalSource?.nativeMuted === true,
    "Cleanup stopped the fixture while retaining its native source mute guard.", {source:report.finalSource});
  const failures = assertions.filter(item => item.status === "fail");
  const skips = assertions.filter(item => item.status === "skip");
  const prerequisites = passed("report-completed") && passed("phase-sequence") && passed("center-audible") &&
    assertions.filter(item => item.id.startsWith("meter:") || item.id.startsWith("same-endpoint:") ||
      item.id.startsWith("source-guard:") || item.id.startsWith("source-playing:")).every(item => item.status === "pass");
  return { schema:1, analyzer:"webview2-production-spatial-audio-measurements", ...metadata,
    mode:remote ? "remote-video" : "constant-tone", status:failures.length ? "fail" : skips.length ? "pass-with-skips" : "pass",
    passed:failures.length === 0, counts:{pass:assertions.length-failures.length-skips.length,fail:failures.length,skip:skips.length},
    claims:{
      permanentNativeMuteGuard:prerequisites && passed("silent:production-route-closed-silent"),
      leftRightPositioning:prerequisites && passed("direction:production-left") && passed("direction:production-right"),
      logicalMuteAndEmptyScene:prerequisites && ["logical-mute","empty-scene","silent:production-logical-muted","silent:production-empty-scene"].every(passed),
      expiredSceneSilence:prerequisites && passed("expired-scene") && passed("silent:production-expired-scene"),
      failureSilence:prerequisites && passed("failure-injected") && passed("silent:production-failed-guard-silent"),
      navigationRecreation:prerequisites && passed("route-recreated") && passed("audible:production-navigation-recreated"),
      navigationRetention:prerequisites && passed("route-retained") && passed("audible:production-navigation-retained"),
      blankStartupWait:prerequisites && passed("about-blank-waits-without-failure"),
      distanceFalloff:prerequisites && passed("distance-falloff"),
    },
    limits:["This standalone harness compiles the shipping route and assets; it does not exercise HostApp integration or game listener transforms.",
      "Failure is injected through the real manager's Fail API; this test does not kill a browser renderer process.",
      "Endpoint measurements include all desktop output. The controlled 997 Hz tone reduces unrelated-audio interference.",
      "There is no collector PCM analyser in the shipping graph; natively muted source plus audible replacement establishes that capture is live.",
      ...(remote ? ["Video soundtrack changes make cross-phase level comparisons observational."] : [])], assertions };
}
