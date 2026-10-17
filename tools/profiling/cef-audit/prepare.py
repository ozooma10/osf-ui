"""Generate a diagnostic helper from current sources, failing on source drift.

Production sources and installed binaries are never modified. Baseline instruments
the current scheduler; copyonly and discard are diagnostic output ablations.
"""
import hashlib
import json
from pathlib import Path
import shutil

REPO = Path(__file__).resolve().parents[3]
OUT = REPO / "build/cef-audit/source"
OUT.mkdir(parents=True, exist_ok=True)
SRC = REPO / "tools/cef_host"
hashes = {}


def replace(text, old, new):
    if text.count(old) != 1:
        raise RuntimeError(f"Expected exactly one source anchor: {old[:100]!r}")
    return text.replace(old, new)


for path in SRC.iterdir():
    if path.is_file():
        shutil.copy2(path, OUT / path.name)
        hashes[str(path.relative_to(REPO))] = hashlib.sha256(path.read_bytes()).hexdigest()
shutil.copy2(Path(__file__).with_name("Audit.h"), OUT / "Audit.h")
text = (SRC / "CMakeLists.txt").read_text()
text = replace(text, 'set(REPO "${CMAKE_CURRENT_SOURCE_DIR}/../..")', f'set(REPO "{REPO.as_posix()}")')
text = replace(text, 'add_subdirectory("${CEF_LIBCEF_DLL_WRAPPER_PATH}" libcef_dll_wrapper)',
               'add_library(libcef_dll_wrapper STATIC IMPORTED)\n'
               f'set_target_properties(libcef_dll_wrapper PROPERTIES IMPORTED_LOCATION "{REPO.as_posix()}/build/cef-host/libcef_dll_wrapper/Release/libcef_dll_wrapper.lib")')
(OUT / "CMakeLists.txt").write_text(text)

text = (SRC / "Host.cpp").read_text()
text = replace(text, '#include "Graphics.h"', '#include "Graphics.h"\n#include "Audit.h"')
text = replace(text, 'command->AppendSwitchWithValue("use-angle", "d3d11");', '''command->AppendSwitchWithValue("use-angle", "d3d11");
            if (audit::Env("OSFUI_CEF_AUDIT_MODE") == "reuse") command->AppendSwitch("process-per-site");''')
text = replace(text, 'std::deque<std::string> queue;', '''struct Queued { std::string text; double arrival; };
        std::deque<Queued> queue;
        double newestPaint{};''')
text = replace(text, 'const bool ok = pipe.WriteMessage(Json::Dump(value))', 'audit::Span span("pipe_send_ms");\n            const bool ok = pipe.WriteMessage(Json::Dump(value))')
text = replace(text, 'output.Stop();\n            closeDeadline', 'output.Stop();\n            audit::traceFinished = [this] { CheckShutdown(); };\n            audit::EndTrace();\n            closeDeadline')
text = replace(text, 'closeDeadline = GetTickCount64() + 5000;', 'closeDeadline = GetTickCount64() + (audit::traceDone ? 5000 : 30000);')
text = replace(text, 'queueBytes += payload.size(); queue.push_back(std::move(payload));', 'queueBytes += payload.size(); queue.push_back({std::move(payload), audit::Now()});')
text = replace(text, 'std::string payload;\n                { std::scoped_lock lock(queueMutex);', 'Queued payload;\n                { std::scoped_lock lock(queueMutex);')
text = replace(text, 'queueBytes -= payload.size();', 'queueBytes -= payload.text.size();')
text = replace(text, 'if (auto value = Json::Parse(payload)) Handle(*value);', '''if (auto value = Json::Parse(payload.text)) {
                    const auto type = Json::Get(*value, "type", "");
                    audit::Record("queue_ms." + type, (audit::Now() - payload.arrival) * 1000);
                    audit::Span span("handle_ms." + type); Handle(*value);
                }''')
text = replace(text, 'else if (type == msg::PostWeb::kType) view->Deliver(msg::FromJson<msg::PostWeb>(raw).json);',
               '''else if (type == msg::PostWeb::kType) {
                const auto text = msg::FromJson<msg::PostWeb>(raw).json;
                if (!audit::Control(text)) view->Deliver(text);
                else {
                    std::uint64_t bytes = 0;
                    for (const auto& [id, item] : views) for (const auto* surface : { &item->surface, &item->popup })
                        if (surface->texture) bytes += std::uint64_t(surface->width) * surface->height * 4;
                    audit::Record("owned_paint_mib", bytes / 1048576.0);
                }
            }''')
text = replace(text, 'if (!open) { tasksAlive->store(false);', 'if (!open && audit::traceDone) { tasksAlive->store(false);')
text = replace(text, 'if (!output.Pending()) return;', 'if (!output.Pending() || audit::NoOutput()) return;\n            audit::Span span("compose_ms");')
text = replace(text, 'output.Published(FramePacer::Clock::now());', '''audit::Count("published"); audit::Interval("publish_interval_ms");
                if (newestPaint) audit::Record("newest_paint_to_publish_ms", (audit::Now() - newestPaint) * 1000);
                output.Published(FramePacer::Clock::now());''')
text = replace(text, '} else output.Blocked();', '} else { output.Blocked(); audit::Count("ring_backpressure"); }')
text = replace(text, 'if (closing || hidden || owner.stopping) return;\n        try {', 'if (closing || hidden || owner.stopping) return;\n        audit::Span span("paint_callback_ms");\n        audit::Interval("paint_interval_ms." + id);\n        audit::Count("paint_count." + id);\n        if (audit::DiscardPaint()) return;\n        try {')
text = replace(text, 'if (type == PET_VIEW) reveal.Painted();', 'owner.newestPaint = audit::Now();\n            if (type == PET_VIEW) reveal.Painted();')
text = replace(text, 'void Application::OnContextInitialized() {', 'void Application::OnContextInitialized() { audit::BeginTrace();')
text = replace(text, 'Host host;\n        host.log.open', 'audit::Init(std::filesystem::path(logPath));\n        Host host;\n        host.log.open')
text = replace(text, 'CefShutdown();\n        app->host', 'audit::Write();\n        CefShutdown();\n        app->host')
(OUT / "Host.cpp").write_text(text)

text = (SRC / "Graphics.cpp").read_text()
text = replace(text, '#include "Graphics.h"', '#include "Graphics.h"\n#include "Audit.h"')
text = replace(text, 'ComPtr<ID3D11Texture2D> source;\n        Check(device1_', 'audit::Span full("copy_total_ms");\n        auto opened = audit::Now();\n        ComPtr<ID3D11Texture2D> source;\n        Check(device1_')
text = replace(text, 'D3D11_TEXTURE2D_DESC desc{};\n        source->GetDesc', 'audit::Record("copy_open_ms", (audit::Now() - opened) * 1000);\n        D3D11_TEXTURE2D_DESC desc{};\n        source->GetDesc')
text = replace(text, 'const D3D11_BOX box{ static_cast<UINT>(copy.x)', '''if (audit::mode == "fullcopy") copy = visible;
        audit::Record("copy_fraction", double(copy.width) * copy.height / (double(visible.width) * visible.height));
        audit::Record("copy_mib", double(copy.width) * copy.height * 4 / 1048576.0);
        const D3D11_BOX box{ static_cast<UINT>(copy.x)''')
text = replace(text, 'context_->CopySubresourceRegion(destination.texture.Get()', 'audit::GpuSpan gpu(device_.Get(), context_.Get(), "copy_gpu_ms");\n        const auto submitted = audit::Now();\n        context_->CopySubresourceRegion(destination.texture.Get()')
text = replace(text, '// CEF may reuse the source after this callback.', 'gpu.End();\n        audit::Record("copy_submit_ms", (audit::Now() - submitted) * 1000);\n        const auto waitStarted = audit::Now();\n        // CEF may reuse the source after this callback.')
text = replace(text, 'if (copyFence_->GetCompletedValue() == (std::numeric_limits<UINT64>::max)())', 'audit::Record("copy_fence_flush_wait_ms", (audit::Now() - waitStarted) * 1000);\n        if (copyFence_->GetCompletedValue() == (std::numeric_limits<UINT64>::max)())')
text = replace(text, 'auto& slot = slots_[index];\n        constexpr', 'auto& slot = slots_[index];\n        audit::GpuSpan gpu(device_.Get(), context_.Get(), "compose_gpu_ms");\n        constexpr')
text = replace(text, 'const auto serial = ++serial_;', 'gpu.End();\n        const auto serial = ++serial_;')
(OUT / "Graphics.cpp").write_text(text)
(OUT / "source-hashes.json").write_text(json.dumps(hashes, indent=2))
print(OUT)
