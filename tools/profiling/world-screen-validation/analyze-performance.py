"""Summarize owned in-game captures; compare matched repeat IDs without pooling frames.

Usage: python analyze-performance.py RUN_OR_CAPTURE [...] --out NEW_DIRECTORY
CPU 100% is one logical core. PresentMon application frames are kept separate from
generated frames, and the main swap chain is selected by application-frame count.
"""
from __future__ import annotations

import argparse
import configparser
import csv
import hashlib
import json
import math
import re
import statistics
import struct
from collections import Counter, defaultdict
from pathlib import Path


def read_json(path):
    return json.loads(Path(path).read_text(encoding="utf-8-sig"))


def number(value):
    try:
        result = float(value)
        return result if math.isfinite(result) else None
    except (ValueError, TypeError):
        return None


def stats(values):
    values = sorted(v for value in values if (v := number(value)) is not None)
    if not values:
        return None

    def percentile(p):
        position = (len(values) - 1) * p
        lower, upper = math.floor(position), math.ceil(position)
        return values[lower] + (values[upper] - values[lower]) * (position - lower)

    median = statistics.median(values)
    return dict(n=len(values), mean=statistics.mean(values), median=median,
                min=values[0], max=values[-1], p95=percentile(.95), p99=percentile(.99),
                mad=statistics.median(abs(value - median) for value in values))


def at(value, *keys):
    for key in keys:
        if not isinstance(value, dict):
            return None
        value = value.get(key)
    return value


def summarize(path):
    manifest = read_json(path / "manifest.json")
    rows = list(csv.DictReader((path / "process.csv").open(encoding="utf-8-sig", newline="")))
    observations = read_json(path / "observations.json")
    kind = manifest.get("captureKind", "steady")
    warnings, invalid = [], []
    run_root = path.parent.parent
    context = read_json(run_root / "context.json")
    preferences = list((run_root / "mo2-cleanup" / "profiles").rglob("StarfieldPrefs.ini"))
    if not preferences:
        preferences = [Path(context["profile"]) / "StarfieldPrefs.ini"]
    rendering = None
    screenshot = run_root / "setup-one-tv.png"
    if len(preferences) == 1 and preferences[0].is_file() and screenshot.is_file():
        ini = configparser.ConfigParser(strict=False)
        ini.read(preferences[0], encoding="utf-8-sig")
        png = screenshot.read_bytes()[:24]
        if png[:8] != b"\x89PNG\r\n\x1a\n":
            raise ValueError(f"Invalid fixture capture: {screenshot}")
        rendering = {"capturedWindowPixels": list(struct.unpack(">II", png[16:24])),
                     "display": {key: value for key, value in ini.items("Display")
                                 if key not in ("ilocation x", "ilocation y", "udisplayhash", "isize w", "isize h")},
                     "quality": dict(ini.items("Quality"))}
        geometry_log = run_root / "logs" / "OSF UI.log"
        if geometry_log.is_file():
            geometries = sorted(set(re.findall(r"geometry mode -> (.*)", geometry_log.read_text(encoding="utf-8-sig"))))
            rendering["runtimeGeometry"] = geometries
            if len(geometries) != 1:
                invalid.append("runtime capture/viewport geometry missing or changed")
        else:
            invalid.append("runtime geometry log unavailable")
    else:
        invalid.append("fixture screenshot or private rendering preferences unavailable")
    if kind == "recovery":
        warnings.append("Transient recovery capture: CPU and memory are descriptive only; compare game-frame gaps with the separately timed recovery evidence.")
    if manifest["status"] != "complete":
        invalid.append("capture did not complete")
    if len(observations) < 3:
        invalid.append("fewer than three observations")
    identities = [frozenset(o["processKeys"]) for o in observations]
    churn = bool(identities and any(keys != identities[0] for keys in identities))
    if churn and kind == "steady":
        invalid.append("process set changed during steady-state measurement")
    documents = {at(o, "page", "documentId") for o in observations}
    if kind == "steady" and (None in documents or len(documents) != 1):
        invalid.append("trusted document missing or changed during steady state")
    stale = sum(1 for o in observations if not at(o, "page", "reportedTickMs") or
                o["systemTickMs"] - o["page"]["reportedTickMs"] > 3000)
    if stale and kind == "steady":
        invalid.append(f"{stale} stale trusted-page observations")
    native = [o.get("native", {}) for o in observations]
    if kind == "steady" and any(not n.get("loaded") or n.get("snapshotStale", True)
                                or n.get("pauseRequestCount") != 0 for n in native):
        invalid.append("game was not continuously fresh, loaded and unpaused")
    game_signature = {key: sorted({json.dumps(n.get(key), sort_keys=True) for n in native})
                      for key in ("foreground", "cellFormId")}
    game_signature["position"] = sorted({json.dumps(
        {axis: round(value, 2) or 0.0 for axis, value in (n.get("position") or {}).items()},
        sort_keys=True) for n in native})
    # The listener follows the camera; stationary player coordinates alone do
    # not detect the automatic vanity orbit. Ignore sub-centimeter idle sway.
    for key in ("listener", "front", "up"):
        game_signature["camera_" + key] = sorted({json.dumps([
            round(value, 2) or 0.0 for value in
            (at(o, "page", "browserState", "audio", "chrome", "spatial", key) or [])
        ]) for o in observations})
    if kind == "steady" and any(len(values) > 1 for values in game_signature.values()):
        invalid.append("game focus, cell, position or camera changed during measurement")
    if observations and at(observations[-1], "page", "frames") == at(observations[0], "page", "frames"):
        warnings.append("trusted page animation counter did not advance")

    process_categories = {}
    for process in manifest.get("processes", []):
        command = process.get("commandLine") or ""
        match = re.search(r"--type=([^\s]+)", command)
        kind_name = match.group(1) if match else "browser"
        category = {"gpu-process": "Gpu", "renderer": "Renderer", "browser": "BrowserMain"}.get(kind_name, "OtherBrowser")
        if "--utility-sub-type=audio.mojom.AudioService" in command:
            category = "AudioService"
        process_categories[str(process["pid"]), process["createdAtUtc"]] = category
    groups = defaultdict(list)
    for row in rows:
        groups[row["role"], row["sample"]].append(row)
        if row["role"] != "Game":
            groups["AllBrowser", row["sample"]].append(row)
        if row["role"] == "Browser":
            category = process_categories.get((row["pid"], row["createdAtUtc"]), "OtherBrowser")
            groups[category, row["sample"]].append(row)
    roles = {}
    for role in ("Game", "Host", "Browser", "AllBrowser", "Gpu", "Renderer", "BrowserMain", "AudioService", "OtherBrowser"):
        samples = []
        for (key_role, _), items in groups.items():
            if key_role != role:
                continue
            cpu = [number(item["cpuPercentOneCore"]) for item in items]
            samples.append(dict(atSeconds=float(items[0]["atSeconds"]),
                                cpu=sum(cpu) if all(v is not None for v in cpu) else None,
                                privateMiB=sum(float(i["privateBytes"]) for i in items) / 2**20,
                                workingSetMiB=sum(float(i["workingSetBytes"]) for i in items) / 2**20,
                                threads=sum(int(i["threads"]) for i in items),
                                handles=sum(int(i["handles"]) for i in items)))
        samples.sort(key=lambda s: s["atSeconds"])
        if samples:
            elapsed = samples[-1]["atSeconds"] - samples[0]["atSeconds"]
            weighted = sum(s["cpu"] * (s["atSeconds"] - p["atSeconds"])
                           for p, s in zip(samples, samples[1:]) if s["cpu"] is not None)
            roles[role] = {field: stats(s[field] for s in samples)
                           for field in ("cpu", "privateMiB", "workingSetMiB", "threads", "handles")}
            roles[role]["cpuTimeWeightedMeanOneCore"] = weighted / elapsed if elapsed else None
            roles[role]["memoryDriftMiB"] = samples[-1]["privateMiB"] - samples[0]["privateMiB"]

    present = None
    csv_path = path / "presentmon.csv"
    if csv_path.exists():
        frames = list(csv.DictReader(csv_path.open(encoding="utf-8-sig", newline="")))
        game_frames = [f for f in frames if f.get("ProcessID") == str(manifest["session"]["pid"])]
        application = [f for f in game_frames if f.get("FrameType", "Application") == "Application"]
        chains = Counter(f.get("SwapChainAddress", "unknown") for f in application)
        if chains:
            main = chains.most_common(1)[0][0]
            selected = [f for f in application if f.get("SwapChainAddress", "unknown") == main]
            metrics = {key: stats(f.get(key) for f in selected) for key in
                       ("FrameTime", "CPUBusy", "CPUWait", "GPUTime", "GPUBusy", "GPUWait", "DisplayLatency")}
            present = dict(rows=len(frames), gameRows=len(game_frames), applicationRows=len(selected),
                           mainSwapChain=main, swapChains=dict(chains),
                           frameTypes=dict(Counter(f.get("FrameType", "unknown") for f in game_frames)),
                           metricsMs=metrics)
            if len(chains) > 1:
                warnings.append("multiple game swap chains; largest application-frame stream selected")
        else:
            invalid.append("PresentMon contains no application frames for the owned game")
    elif manifest.get("presentMon"):
        invalid.append("requested PresentMon CSV missing")
    else:
        warnings.append("CPU-only capture; game frame-pacing improvement is unmeasured")

    metrics = {f"{role}.cpuOneCore": value["cpuTimeWeightedMeanOneCore"] for role, value in roles.items()}
    for role, value in roles.items():
        metrics[f"{role}.privateMiB"] = value["privateMiB"]["mean"]
    if observations:
        first, last = observations[0].get("page") or {}, observations[-1].get("page") or {}
        elapsed = ((number(last.get("at")) or 0) - (number(first.get("at")) or 0)) / 1000
        if elapsed > 0 and first.get("documentId") == last.get("documentId"):
            for key in ("stateEvents", "audioOnlyEvents", "domMutations"):
                before, after = number(at(first, "diagnostics", key)), number(at(last, "diagnostics", key))
                if before is not None and after is not None and after >= before:
                    metrics[f"Chrome.{key}PerSecond"] = (after - before) / elapsed
        heap = [number(at(o, "page", "diagnostics", "jsHeapUsed")) for o in observations]
        heap = [value / 2**20 for value in heap if value is not None]
        if heap:
            metrics["Chrome.jsHeapUsedMiB"] = statistics.mean(heap)
    if present:
        for key in ("FrameTime", "CPUBusy", "GPUBusy"):
            if present["metricsMs"].get(key):
                for stat in ("mean", "p95", "p99", "max"):
                    metrics[f"{key}.{stat}Ms"] = present["metricsMs"][key][stat]
    snapshots = [at(o, "page", "browserState") for o in observations]
    audio = {}
    for route in ((), ("chrome",)):
        name = "content" if not route else "chrome"
        values = [at(s, "audio", *route) for s in snapshots]
        audio[name] = {field: [at(v, field) for v in (values[0], values[-1])] if values else []
                       for field in ("packets", "frames", "droppedPackets", "generation", "error")}
        audio[name]["emitterCounts"] = sorted({count for v in values
                                                if (count := at(v, "spatial", "emitterCount")) is not None})
    signature = {key: sorted({json.dumps(at(s, key), sort_keys=True) for s in snapshots})
                 for key in ("url", "fullscreen", "landing", "loading", "muted")}
    signature["fixtureTone"] = sorted({json.dumps({key: at(o, "page", "audioProbe", key)
                                                  for key in ("state", "toneHz")}, sort_keys=True)
                                       for o in observations})
    signature["emitters"] = sorted({json.dumps(at(s, "audio", "chrome", "spatial", "emitterCount"))
                                    for s in snapshots})
    if kind == "steady" and any(len(values) > 1 for values in signature.values()):
        invalid.append("browser workload state changed during measurement")
    return dict(capture=str(path), backend=manifest["backend"], variant=manifest["variant"],
                workload=manifest["workload"], repeat=manifest["repeat"], runId=manifest["runId"],
                valid=not invalid, invalid=invalid, warnings=warnings, metrics=metrics, captureKind=kind,
                processChurn=churn, stalePageObservations=stale,
                roles=roles, presentMon=present, audio=audio, stateSignature=signature,
                gameSignature=game_signature,
                renderConfiguration=rendering,
                durationSeconds=manifest["durationSeconds"], intervalMs=manifest["intervalMs"],
                machine=manifest.get("machine"), hashes=manifest["hashes"],
                manifestSha256=hashlib.sha256((path / "manifest.json").read_bytes()).hexdigest())


def compare(runs):
    grouped = defaultdict(dict)
    for run in runs:
        key = run["backend"], run["workload"]
        trial = run["variant"], run["repeat"]
        if trial in grouped[key]:
            raise ValueError(f"Duplicate capture for {key}/{trial}; select one explicit experiment.")
        grouped[key][trial] = run
    comparisons = []
    for (backend, workload), group in sorted(grouped.items()):
        repeats = sorted({repeat for _, repeat in group})
        pairs, excluded = [], []
        for repeat in repeats:
            baseline, current = group.get(("baseline", repeat)), group.get(("current", repeat))
            if not baseline or not current:
                excluded.append(dict(repeat=repeat, reason="unpaired baseline/current repeat"))
                continue
            if not baseline["valid"] or not current["valid"]:
                excluded.append(dict(repeat=repeat, reason="capture invalid", baseline=baseline["invalid"], current=current["invalid"]))
                continue
            matched_fields = ["durationSeconds", "intervalMs", "machine", "captureKind", "renderConfiguration"]
            if baseline["captureKind"] == "steady":
                matched_fields.extend(("stateSignature", "gameSignature"))
            for key in matched_fields:
                if baseline[key] != current[key]:
                    excluded.append(dict(repeat=repeat, reason=f"mismatched {key}"))
                    break
            else:
                pairs.append((baseline, current))
        metrics = {}
        names = sorted(set().union(*(set(a["metrics"]) & set(b["metrics"]) for a, b in pairs))) if pairs else []
        if pairs and pairs[0][0]["captureKind"] == "recovery":
            names = [name for name in names if name.startswith("FrameTime.")]
        for name in names:
            before, after, deltas = [], [], []
            for baseline, current in pairs:
                a, b = baseline["metrics"][name], current["metrics"][name]
                if a is None or b is None:
                    continue
                before.append(a); after.append(b); deltas.append(b - a)
            if not deltas:
                continue
            a, b = stats(before), stats(after)
            metrics[name] = dict(baseline=a, current=b, pairedDelta=stats(deltas),
                                 medianChangePercent=100*(b["median"]-a["median"])/a["median"] if a["median"] else None,
                                 allPairsLower=all(d < 0 for d in deltas), allPairsHigher=all(d > 0 for d in deltas))
        comparisons.append(dict(backend=backend, workload=workload, pairedRepeats=len(pairs),
                                excluded=excluded, metrics=metrics,
                                interpretation="Descriptive repeated measurements, not proof of statistical significance; negative cost deltas are better."))
    return comparisons


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("paths", nargs="+", type=Path)
    parser.add_argument("--out", required=True, type=Path)
    args = parser.parse_args()
    paths = set()
    for root in args.paths:
        if (root / "manifest.json").is_file() and (root / "process.csv").is_file():
            paths.add(root.resolve())
        else:
            paths.update(p.parent.resolve() for p in root.rglob("performance/*/manifest.json"))
    if not paths:
        parser.error("No performance captures found")
    runs = [summarize(path) for path in sorted(paths)]
    comparison = compare(runs)
    args.out.mkdir(parents=True, exist_ok=False)
    result = dict(schemaVersion=1, runs=runs, comparisons=comparison,
                  limits=["CPU 100% is one logical core; game process CPU is not OSF UI attribution.",
                          "Working set may double-count shared pages; private memory excludes VRAM.",
                          "No context-switch or GPU-engine sampling; game frame timing comes from PresentMon when present.",
                          "Warmup, save/camera/display/frame-cap parity and focus must be established by the run operator.",
                          "No workload mutations or audio output measurements are performed by this sampler."])
    (args.out / "summary.json").write_text(json.dumps(result, indent=2), encoding="utf-8")
    text = ["# World-screen in-game performance", "", "Costs use 100% CPU = one logical core. Values are medians across matched repeats; ranges show observed run-to-run variation.", "",
            "| Backend / workload | Paired repeats | Metric | Baseline median [min, max] | Current median [min, max] | Change |", "| --- | ---: | --- | ---: | ---: | ---: |"]
    for item in comparison:
        for metric in ("Game.cpuOneCore", "Host.cpuOneCore", "Browser.cpuOneCore", "AllBrowser.cpuOneCore",
                       "AllBrowser.privateMiB", "Gpu.privateMiB", "Renderer.privateMiB",
                       "Chrome.stateEventsPerSecond", "Chrome.audioOnlyEventsPerSecond", "Chrome.domMutationsPerSecond",
                       "Chrome.jsHeapUsedMiB", "FrameTime.p99Ms", "FrameTime.maxMs"):
            value = item["metrics"].get(metric)
            if not value:
                continue
            a, b = value["baseline"], value["current"]
            change = value["medianChangePercent"]
            text.append(f"| {item['backend']} / {item['workload']} | {item['pairedRepeats']} | {metric} | {a['median']:.3f} [{a['min']:.3f}, {a['max']:.3f}] | {b['median']:.3f} [{b['min']:.3f}, {b['max']:.3f}] | {change:+.1f}% |" if change is not None else
                        f"| {item['backend']} / {item['workload']} | {item['pairedRepeats']} | {metric} | {a['median']:.3f} | {b['median']:.3f} | n/a |")
    text.extend(["", "Unpaired or invalid captures are retained in summary.json and excluded from paired comparisons. Consistent direction across three repeats is descriptive evidence, not a significance test.", ""])
    for run in runs:
        if run["invalid"]:
            text.append(f"- Excluded {run['capture']}: {'; '.join(run['invalid'])}.")
    (args.out / "summary.md").write_text("\n".join(text) + "\n", encoding="utf-8")
    print(json.dumps(dict(captures=len(runs), valid=sum(r["valid"] for r in runs),
                          pairedRepeats=sum(c["pairedRepeats"] for c in comparison), output=str(args.out)), indent=2))


if __name__ == "__main__":
    main()
