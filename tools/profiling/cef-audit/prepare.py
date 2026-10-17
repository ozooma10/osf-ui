"""Generate a diagnostic helper from current sources, failing on source drift.

Production sources and installed binaries are never modified. The three modes
share one instrumented binary: baseline, immediate messages, coalesced events.
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
text = replace(text, 'std::deque<std::string> queue;', '''struct Queued { std::string text; double arrival; };
        std::deque<Queued> queue;
        std::atomic_bool messageWake{};
        bool composeWake{};
        double newestPaint{};
        void WakeMessages() {
            if (audit::EventMessages() && !messageWake.exchange(true))
                CefPostTask(TID_UI, new Task([this] { messageWake = false; DrainMessages(); if (audit::EventPaint()) Compose(); }));
        }
        void WakeCompose() {
            if (audit::EventPaint() && !composeWake) {
                composeWake = true;
                CefPostTask(TID_UI, new Task([this] { composeWake = false; Compose(); }));
            }
        }
        void DrainMessages() {
            for (int count = 0; !stopping && count < 128; ++count) {
                Queued value;
                { std::scoped_lock lock(queueMutex); if (queue.empty()) break;
                  value = std::move(queue.front()); queue.pop_front(); queueBytes -= value.text.size(); }
                if (auto parsed = Json::Parse(value.text)) {
                    const auto type = Json::Get(*parsed, "type", "");
                    audit::Record("queue_ms." + type, (audit::Now() - value.arrival) * 1000);
                    audit::Span span("handle_ms." + type); Handle(*parsed);
                }
            }
        }''')
text = replace(text, 'const bool ok = pipe.WriteMessage(Json::Dump(value))', 'audit::Span span("pipe_send_ms");\n            const bool ok = pipe.WriteMessage(Json::Dump(value))')
text = replace(text, 'stopping = true;\n            closeDeadline', 'stopping = true;\n            audit::Write();\n            audit::EndTrace();\n            closeDeadline')
text = replace(text, 'closeDeadline = GetTickCount64() + 5000;', 'closeDeadline = GetTickCount64() + (audit::traceDone ? 5000 : 30000);')
text = replace(text, 'queueBytes += payload.size(); queue.push_back(std::move(payload));', 'queueBytes += payload.size(); queue.push_back({std::move(payload), audit::Now()});\n                WakeMessages();')
text = replace(text, 'else if (type == msg::PostWeb::kType) view->Deliver(msg::FromJson<msg::PostWeb>(raw).json);',
               'else if (type == msg::PostWeb::kType) { const auto text = msg::FromJson<msg::PostWeb>(raw).json; if (!audit::Control(text)) view->Deliver(text); }')
start = text.index('                for (int count = 0; !stopping && count < 128; ++count) {', text.index('        void Pump()'))
end = text.index('                Compose();', start)
text = text[:start] + '                DrainMessages();\n' + text[end:]
text = replace(text, 'void Pump()\n        {\n            try {', 'void Pump()\n        {\n            audit::Interval("pump_interval_ms");\n            audit::Span span("pump_ms");\n            try {')
text = replace(text, 'if (!open) { CefQuitMessageLoop(); return; }', 'if (!open && audit::traceDone) { CefQuitMessageLoop(); return; }')
text = replace(text, 'if (!dirty || stopping) return;', 'if (!dirty || stopping || audit::NoOutput()) return;\n            audit::Span span("compose_ms");')
text = replace(text, 'if (graphics.Publish(layers, width, height, epoch)) dirty = false;', '''if (graphics.Publish(layers, width, height, epoch)) {
                dirty = false; audit::Count("published"); audit::Interval("publish_interval_ms");
                if (newestPaint) audit::Record("newest_paint_to_publish_ms", (audit::Now() - newestPaint) * 1000);
            } else audit::Count("ring_backpressure");''')
text = replace(text, 'if (closing || owner.stopping) return;\n        try {', 'if (closing || owner.stopping) return;\n        audit::Span span("paint_callback_ms");\n        audit::Interval("paint_interval_ms." + id);\n        audit::Count("paint_count." + id);\n        if (audit::DiscardPaint()) return;\n        try {')
text = replace(text, 'owner.dirty = true;\n        } catch (const GraphicsError& error)', 'owner.dirty = true; owner.newestPaint = audit::Now(); owner.WakeCompose();\n        } catch (const GraphicsError& error)')
text = replace(text, 'void Application::OnContextInitialized() { if (host) host->Pump(); }', 'void Application::OnContextInitialized() { audit::BeginTrace(); if (host) host->Pump(); }')
text = replace(text, 'Host host;\n        host.log.open', 'audit::Init(std::filesystem::path(logPath));\n        Host host;\n        host.log.open')
text = replace(text, 'CefShutdown();\n        app->host', 'audit::Write();\n        CefShutdown();\n        app->host')
(OUT / "Host.cpp").write_text(text)

text = (SRC / "Graphics.cpp").read_text()
text = replace(text, '#include "Graphics.h"', '#include "Graphics.h"\n#include "Audit.h"')
text = replace(text, 'ComPtr<ID3D11Texture2D> source;\n        Check(device1_', 'audit::Span full("copy_total_ms");\n        auto opened = audit::Now();\n        ComPtr<ID3D11Texture2D> source;\n        Check(device1_')
text = replace(text, 'D3D11_TEXTURE2D_DESC desc{};\n        source->GetDesc', 'audit::Record("copy_open_ms", (audit::Now() - opened) * 1000);\n        D3D11_TEXTURE2D_DESC desc{};\n        source->GetDesc')
text = replace(text, 'context_->CopySubresourceRegion(destination.texture.Get()', 'audit::GpuSpan gpu(device_.Get(), context_.Get(), "copy_gpu_ms");\n        const auto submitted = audit::Now();\n        context_->CopySubresourceRegion(destination.texture.Get()')
text = replace(text, '// CEF may reuse the source after this callback.', 'gpu.End();\n        audit::Record("copy_submit_ms", (audit::Now() - submitted) * 1000);\n        const auto waitStarted = audit::Now();\n        // CEF may reuse the source after this callback.')
text = replace(text, 'if (copyFence_->GetCompletedValue() == (std::numeric_limits<UINT64>::max)())', 'audit::Record("copy_fence_flush_wait_ms", (audit::Now() - waitStarted) * 1000);\n        if (copyFence_->GetCompletedValue() == (std::numeric_limits<UINT64>::max)())')
text = replace(text, 'auto& slot = slots_[index];\n        constexpr', 'auto& slot = slots_[index];\n        audit::GpuSpan gpu(device_.Get(), context_.Get(), "compose_gpu_ms");\n        constexpr')
text = replace(text, 'const auto serial = ++serial_;', 'gpu.End();\n        const auto serial = ++serial_;')
(OUT / "Graphics.cpp").write_text(text)
(OUT / "source-hashes.json").write_text(json.dumps(hashes, indent=2))
print(OUT)
