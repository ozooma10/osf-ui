"""Summarize retained audit runs. Trace analysis is optional and kept separate."""
import argparse
from collections import defaultdict
import html
import json
from pathlib import Path
import statistics

parser = argparse.ArgumentParser()
parser.add_argument("roots", nargs="+")
parser.add_argument("--out", required=True)
parser.add_argument("--trace", help="Completed Chromium trace run directory")
args = parser.parse_args()
rows = []
for root in map(Path, args.roots):
    metadata = json.loads((root / "metadata.json").read_text(encoding="utf-8-sig"))
    for run in sorted(root.iterdir()):
        if not (run / "result.json").exists():
            continue
        result = json.loads((run / "result.json").read_text())
        audit = json.loads((run / "audit.json").read_text()) if (run / "audit.json").exists() else {}
        for phase in result["phases"]:
            row = {"run": str(run.resolve()), "case": run.name.split("-")[0], "height": result["height"],
                   "traced": metadata["trace"], "gpu_queries": metadata["gpuQueries"],
                   "diagnostic_no_output": result.get("diagnostic_no_output", False),
                   "passed": result.get("status") == "passed" and result.get("exit_code") == 0,
                   **{key: phase[key] for key in ("name", "seconds", "unique_fps", "delivered_fps", "cpu_core_percent", "root_cpu_core_percent", "private_commit_mean_mib", "bridge_rtt_p50_ms", "bridge_rtt_p95_ms", "consumer_copy_p95_ms", "invalid_markers")}}
            row["child_cpu_core_percent"] = row["cpu_core_percent"] - row["root_cpu_core_percent"]
            row["metrics"] = audit.get("phases", {}).get(phase["name"], {})
            rows.append(row)
groups = defaultdict(list)
for row in rows:
    if row["passed"]:
        groups[(row["height"], row["case"], row["name"], row["traced"], row["gpu_queries"])].append(row)
medians = []
for (height, case, phase, traced, gpu), group in sorted(groups.items()):
    summary = dict(height=height, case=case, phase=phase, traced=traced, gpu_queries=gpu, runs=len(group))
    keys = ("unique_fps", "delivered_fps", "cpu_core_percent", "root_cpu_core_percent", "child_cpu_core_percent", "private_commit_mean_mib", "bridge_rtt_p50_ms", "bridge_rtt_p95_ms", "consumer_copy_p95_ms")
    summary.update({key: statistics.median(row[key] for row in group) for key in keys})
    summary["fps_range"] = [min(row["unique_fps"] for row in group), max(row["unique_fps"] for row in group)]
    summary["metrics"] = {}
    for key in sorted(set().union(*(row["metrics"].keys() for row in group))):
        values = [row["metrics"][key] for row in group if key in row["metrics"]]
        summary["metrics"][key] = {field: statistics.median(value[field] for value in values) for field in ("count", "sum", "mean", "p50", "p95", "max")}
    medians.append(summary)
out = Path(args.out)
out.mkdir(parents=True, exist_ok=True)
payload = {"rows": rows, "medians": medians}
if args.trace:
    run = Path(args.trace)
    trace = json.loads((run / "chromium-trace.json").read_text())
    audit = json.loads((run / "audit.json").read_text())
    result = json.loads((run / "result.json").read_text())
    events = trace["traceEvents"]
    processes, threads = {}, {}
    for event in events:
        if event.get("ph") == "M":
            if event["name"] == "process_name": processes[str(event["pid"])] = event.get("args", {}).get("name", "")
            if event["name"] == "thread_name": threads[f'{event["pid"]}:{event["tid"]}'] = event.get("args", {}).get("name", "")
    processes.setdefault(str(result["pid"]), "CEF browser / OSF host")
    phases = {}
    boundaries = audit["boundaries"]
    for index, boundary in enumerate(boundaries[:-1]):
        name = boundary["phase"]
        if name == "unmeasured": continue
        start, end = boundary["trace_us"], boundaries[index + 1]["trace_us"]
        sums = defaultdict(lambda: [0, 0.0])
        for event in events:
            if event.get("ph") != "X" or "dur" not in event: continue
            a, b = max(start, event["ts"]), min(end, event["ts"] + event["dur"])
            if b <= a: continue
            key = (str(event["pid"]), str(event["tid"]), event["name"])
            sums[key][0] += 1; sums[key][1] += (b - a) / 1000
        tasks = [dict(pid=pid, process=processes.get(pid, "unknown"), thread=threads.get(f"{pid}:{tid}", tid), name=event_name, count=count, inclusive_wall_ms=wall)
                 for (pid, tid, event_name), (count, wall) in sums.items()]
        measured = next(p for p in result["phases"] if p["name"] == name)
        cpu = []
        for pid, final in measured["process_final"].items():
            seconds = final["cpu_seconds"] - measured["process_baseline"].get(pid, {}).get("cpu_seconds", 0)
            cpu.append(dict(pid=pid, process=processes.get(pid, "unknown"), cpu_core_percent=seconds / measured["seconds"] * 100))
        phases[name] = dict(seconds=(end - start) / 1e6, process_cpu=cpu,
                            scopes=sorted(tasks, key=lambda item: -item["inclusive_wall_ms"])[:120])
    payload["trace"] = {"run": str(run.resolve()), "events": len(events), "processes": processes, "threads": threads, "phases": phases,
                         "caveat": "Trace scopes are inclusive wall time, overlap/nest, and must not be summed as CPU. Tracing materially changes performance; CPU attribution applies to this traced run only."}
(out / "summary.json").write_text(json.dumps(payload, indent=2))
body = []
for row in medians:
    fields = [row["height"], row["case"], row["phase"], row["runs"], f'{row["unique_fps"]:.1f}', f'{row["cpu_core_percent"]:.1f}', f'{row["root_cpu_core_percent"]:.1f}', f'{row["child_cpu_core_percent"]:.1f}', f'{row["bridge_rtt_p95_ms"]:.2f}', f'{row["private_commit_mean_mib"]:.0f}', row["traced"], row["gpu_queries"]]
    body.append("<tr>" + "".join(f"<td>{html.escape(str(v))}</td>" for v in fields) + "</tr>")
headers = ["Height", "Variant", "Workload", "Runs", "New frames/s", "CPU %", "Host CPU %", "Children CPU %", "Bridge p95 ms", "Private MiB", "Traced", "GPU queries"]
page = '<!doctype html><meta charset="utf-8"><title>CEF performance audit</title><style>body{font:15px system-ui;margin:36px;background:#101722;color:#e0e9f4}table{border-collapse:collapse;width:100%}td,th{padding:8px;border-bottom:1px solid #344255;text-align:right}th{color:#8cd7ff}p{max-width:1000px;line-height:1.6}a{color:#8cd7ff}</style><h1>CEF performance audit</h1><p>Medians across each group. CPU 100% means one logical core. Host CPU includes both OSF code and CEF browser-process internals; children are Chromium renderer/GPU/utility processes. Private memory is committed memory, not VRAM. Trace and GPU-query runs are separate diagnostic groups. Raw runs and timing breakdowns are in <a href="summary.json">summary.json</a>.</p><table><thead><tr>' + ''.join(f'<th>{h}</th>' for h in headers) + '</tr></thead><tbody>' + ''.join(body) + '</tbody></table>'
(out / "comparison.html").write_text(page)
for row in medians:
    print(f'{row["height"]:4} {row["case"]:9} {row["phase"]:11} n={row["runs"]} fps={row["unique_fps"]:5.1f} cpu={row["cpu_core_percent"]:5.1f} host={row["root_cpu_core_percent"]:5.1f} rtt95={row["bridge_rtt_p95_ms"]:6.2f}')
