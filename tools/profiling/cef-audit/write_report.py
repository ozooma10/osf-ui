"""Build a readable findings report from completed audit evidence."""
import html
import json
from pathlib import Path
import sys

root = Path(sys.argv[1])
data = json.loads((root / "summary.json").read_text())


def group(case, phase, height=1080, gpu=False):
    return next(r for r in data["medians"] if r["case"] == case and r["phase"] == phase and r["height"] == height and r["gpu_queries"] == gpu and not r["traced"])


def table(headers, rows):
    return '<table><thead><tr>' + ''.join(f'<th>{html.escape(str(x))}</th>' for x in headers) + '</tr></thead><tbody>' + ''.join('<tr>' + ''.join(f'<td>{html.escape(str(x))}</td>' for x in row) + '</tr>' for row in rows) + '</tbody></table>'


def number(value): return f"{value:.2f}"


performance, cpu, timing, ablation = [], [], [], []
for phase in ("canvas", "dom", "three_views"):
    base, messages, events = (group(mode, phase) for mode in ("baseline", "messages", "events"))
    performance.append([phase, f'{base["unique_fps"]:.1f}', f'{events["unique_fps"]:.1f}', number(base["bridge_rtt_p95_ms"]), number(messages["bridge_rtt_p95_ms"]), number(events["bridge_rtt_p95_ms"])])
    cpu.append([phase, number(base["cpu_core_percent"]), number(events["cpu_core_percent"]), number(events["root_cpu_core_percent"]), number(events["child_cpu_core_percent"]), f'{events["private_commit_mean_mib"]:.0f}'])
    g = group("baseline", phase, gpu=True)
    gm, m = g["metrics"], base["metrics"]
    timing.append([phase, number(gm["copy_gpu_ms"]["mean"] * 1000), number(gm["compose_gpu_ms"]["mean"] * 1000), number(m["copy_fence_flush_wait_ms"]["mean"]), number(m["paint_callback_ms"]["mean"])])
    a, b = group("copyonly", phase), group("discard", phase)
    ablation.append([phase, number(base["cpu_core_percent"]), number(a["cpu_core_percent"]), number(b["cpu_core_percent"]), number(b["root_cpu_core_percent"]), number(b["child_cpu_core_percent"])])

functional = [r for r in data["rows"] if r["passed"] and not r["diagnostic_no_output"]]
runs = {r["run"] for r in data["rows"] if r["passed"]}
three = group("events", "three_views")
rates = [r["metrics"].get("published", {}).get("count", 0) / r["seconds"] for r in data["rows"] if r["case"] == "events" and r["height"] == 1080 and r["name"] == "three_views"]
range_rows = [[r["case"], r["phase"], r["runs"], f'{r["fps_range"][0]:.1f}–{r["fps_range"][1]:.1f}'] for r in data["medians"] if r["height"] == 1080 and r["case"] in ("baseline", "messages", "events") and not r["gpu_queries"] and r["phase"] != "idle"]
check1440 = []
for phase in ("canvas", "dom", "three_views"):
    a, b = group("baseline", phase, 1440), group("events", phase, 1440)
    check1440.append([phase, number(a["unique_fps"]), number(b["unique_fps"]), number(a["bridge_rtt_p95_ms"]), number(b["bridge_rtt_p95_ms"]), number(a["cpu_core_percent"]), number(b["cpu_core_percent"])])

page = '''<!doctype html><html><head><meta charset="utf-8"><title>CEF performance attribution audit</title>
<style>body{font:16px system-ui;max-width:1180px;margin:40px auto;padding:0 24px;background:#101722;color:#e3eaf3;line-height:1.6}h1{font-size:34px;line-height:1.2}h2{font-size:23px;margin-top:36px;color:#8cd7ff}a{color:#8cd7ff}table{border-collapse:collapse;width:100%;font-variant-numeric:tabular-nums;margin:20px 0}td,th{padding:9px 12px;border-bottom:1px solid #354052;text-align:right}td:first-child,th:first-child{text-align:left}th{font-size:13px;color:#a9bdd6}p,li{max-width:1040px}.lead{font-size:20px;background:#182b3a;padding:22px;border-left:4px solid #62d4c4}.note{color:#b0bfd1;font-size:14px}code{color:#99e0d7}li{margin:12px 0}</style></head><body>
<h1>CEF performance attribution audit</h1><p class="note">17 October 2026 · RTX 3080 · CEF 150.0.22 · desktop helper diagnostics</p>
<p class="lead">Our scheduling is the main confirmed cause of the prototype’s high bridge latency and lost output frames. Changing only dispatch/publication restores approximately 60 new frames per second. Most remaining CPU runs in Chromium’s renderer/GPU processes, including the page’s own JavaScript and rendering work; that is not all CEF framework overhead.</p>
<p>The installed backend and production helper sources were left unchanged. Diagnostic builds preserve sandboxing, shared textures and borrowed-texture copy completion. All comparisons below use untraced runs unless explicitly labeled otherwise.</p>
<h2>1. Controlled scheduling comparison</h2><p>The baseline polls messages and publishes frames from a task scheduled 16 ms after the previous task. The <code>messages</code> variant immediately posts queued input to CEF’s UI thread. The <code>events</code> variant additionally publishes dirty frames from coalesced UI tasks. Same CEF binary, workload, dimensions and copy-lifetime rules within each trial set.</p>'''
page += table(["Workload", "Baseline FPS", "Events FPS", "Baseline RTT p95 ms", "Messages RTT p95 ms", "Events RTT p95 ms"], performance)
page += '<p>Three runs per functional variant at 1080p, six measured seconds per workload after two seconds of warmup. CEF paint callbacks already arrived at about 60 Hz while baseline output fell behind. Immediate message dispatch fixes latency without fixing frame delivery; changing publication scheduling fixes the latter. This isolates two OSF integration costs.</p>'
page += '<h2>2. CPU location</h2><p>CPU 100% means one logical core. The root process contains both OSF native code and CEF browser-process internals, so its CPU is an upper bound on direct OSF helper CPU. Child processes run Chromium rendering, GPU command handling, JavaScript and utilities.</p>'
page += table(["Workload", "Baseline CPU %", "Events CPU %", "Events host %", "Events children %", "Events private MiB"], cpu)
page += '<p>CPU varies more than the latency/frame-delivery results. The experiment demonstrates better output delivery; it does not establish a precise universal CPU saving. The completed Chromium trace identifies animation JavaScript in Canvas workloads, style/layout/paint in the DOM workload, and raster command processing in the GPU process. Page work and Chromium infrastructure remain inseparable as a universal “CEF tax” in this test.</p>'
page += '<h2>3. Copy cost versus waiting</h2><p>GPU columns use a separate untraced D3D11 timestamp-query run, three measured seconds per workload. Wall-time columns use the three lightweight baseline trials without GPU queries. GPU timings cover only our own copy/compositor commands, not total Chromium GPU rendering. Query collection itself increased CPU cost, so that diagnostic run is not part of the throughput/CPU comparison.</p>'
page += table(["Workload", "Copy GPU µs", "Composition GPU µs", "Signal/flush/wait ms", "Paint callback ms"], timing)
page += '<p>The synchronous handoff exposes completion/scheduling delay on CEF’s UI thread; it is not equivalent to expensive pixel-copy execution. The event-driven variant reaches its target while retaining this wait. It is therefore a secondary optimization candidate, not the first fix. CEF requires the borrowed resource to stay within the callback; blindly removing the completion wait would violate the ownership guarantee. See the <a href="https://github.com/chromiumembedded/cef/blob/master/include/cef_render_handler.h">CEF render-handler contract</a>.</p>'
page += '<h2>4. Remove integration work to bound its cost</h2><p>One exploratory run each: <code>copyonly</code> retains paint copies but suppresses composition/publication during measurement; <code>discard</code> returns from paint callbacks without opening/copying textures and suppresses output. The same pages and CEF rendering remain active. These modes intentionally produce no usable output; they are diagnostic controls, not proposed optimizations. The baseline column is the three-run median, not a claim of precision for these single controls.</p>'
page += table(["Workload", "Baseline CPU %", "Copy only %", "Discard %", "Discard host %", "Discard children %"], ablation)
page += '<h2>5. Practical priorities</h2><ol><li><strong>Replace queue polling with immediate, coalesced UI dispatch.</strong> Preserve queue limits and shutdown/lifetime behavior.</li><li><strong>Publish on paint/ack readiness.</strong> Keep latest-frame behavior and coalesce multiple view updates into the intended display cadence.</li>'
page += f'<li><strong>Avoid redundant multi-view publications.</strong> The immediate-publication experiment issued approximately {min(rates):.0f}–{max(rates):.0f} output frames/s for three views, while only about 60 top-view frames/s were new. This is a remaining integration inefficiency even though its observed GPU work is small.</li>'
page += '<li><strong>Profile completion waits before redesigning texture ownership.</strong> Handle caching or simply removing the wait is not valid under the current CEF contract.</li><li><strong>Treat page rendering as a separate optimization target.</strong> The synthetic DOM workload changes widths every animation frame and triggers layout. That is workload cost, not evidence that OSF message handling is expensive.</li></ol>'
page += '<h2>1440p check</h2><p>One run per variant; corroborating evidence rather than a stable estimate of variance. Frame delivery improves, but CPU is higher with event-driven output in this pair. The scheduling change is not proven to lower CPU universally; producing more output and the redundant multi-view publications can add work.</p>' + table(["Workload", "Baseline FPS", "Events FPS", "Baseline RTT p95 ms", "Events RTT p95 ms", "Baseline CPU %", "Events CPU %"], check1440)
page += '<h2>1080p delivery ranges</h2>' + table(["Variant", "Workload", "Runs", "FPS min–max"], range_rows)
page += f'<h2>Evidence and limitations</h2><p>{len(runs)} completed untraced audit sessions across controls, repeats, output ablations, 1440p and GPU timing. Functional runs returned valid frame markers and helpers exited with code zero. The earlier trace smoke is separate: baseline tracing completed, while the heavily perturbed event trace hit a shutdown timeout and is excluded. This also exposed and fixed a benchmark reporting issue: a process exiting nonzero now fails the run instead of being labeled clean merely because it exited.</p>'
page += '<ul><li>Full tracing materially increases CPU and can alter frame cadence; traced data is used only to identify work. Inclusive trace scopes overlap and must not be summed as CPU samples.</li><li>This desktop consumer performs synchronous GPU readback. Starfield’s D3D12 compositor, real input, frame generation, lifecycle and background behavior still need in-game validation.</li><li>WebView2 155 and CEF 150 differ in Chromium version and output path. The audit does not isolate an inherent engine/framework overhead difference.</li><li>Process CPU excludes the benchmark consumer and DWM. Private commitment excludes VRAM. Three opaque overlapping views validate only the topmost pixel marker.</li><li>Two diagnostic revisions were used: output-ablation branches were added after the first control set. Baseline/messages/events behavior and timing points were preserved. Generated sources, DLLs and hashes are retained per set.</li></ul>'
page += '<p><a href="summary.json">Full measurements and trace attribution JSON</a> · <a href="comparison.html">All variant tables</a> · <a href="../../../tools/profiling/cef-audit/README.md">Reproduction instructions</a></p></body></html>'
(root / "audit-report.html").write_text(page, encoding="utf-8")
print(root / "audit-report.html")
