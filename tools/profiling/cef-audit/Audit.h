#pragma once
// Included only by generated diagnostic sources, never by the shipped helper.
#include <Windows.h>
#include <d3d11.h>
#include <wrl/client.h>
#include <algorithm>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <map>
#include <string>
#include <vector>
#include <nlohmann/json.hpp>
#include "include/cef_trace.h"

namespace audit {
using json = nlohmann::json;
inline double Now() { LARGE_INTEGER n{}, f{}; QueryPerformanceCounter(&n); QueryPerformanceFrequency(&f); return double(n.QuadPart) / f.QuadPart; }
inline std::string Env(const char* name) { char text[256]{}; GetEnvironmentVariableA(name, text, sizeof(text)); return text; }
inline std::string mode = "baseline", phase = "unmeasured";
inline bool traceStarted{}, traceDone = true, gpuEnabled{};
inline std::filesystem::path directory;
inline json boundaries = json::array();
inline std::map<std::string, std::map<std::string, std::vector<double>>> metrics;
inline std::map<std::string, double> previous;
inline void RecordFor(const std::string& p, const std::string& name, double ms) { if (p != "unmeasured") metrics[p][name].push_back(ms); }
inline void Record(const std::string& name, double ms) { RecordFor(phase, name, ms); }
inline void Count(const std::string& name) { Record(name, 1); }
inline void Interval(const std::string& name) { auto now = Now(); auto old = previous[name]; previous[name] = now; if (old) Record(name, (now - old) * 1000); }
struct Span {
    std::string p, name; double start;
    explicit Span(std::string n) : p(phase), name(std::move(n)), start(Now()) {}
    ~Span() { RecordFor(p, name, (Now() - start) * 1000); }
};
inline bool EventMessages() { return mode == "messages" || mode == "events"; }
inline bool EventPaint() { return mode == "events"; }
inline bool NoOutput() { return phase != "unmeasured" && (mode == "copyonly" || mode == "discard"); }
inline bool DiscardPaint() { return phase != "unmeasured" && mode == "discard"; }
inline void Init(const std::filesystem::path& log) {
    directory = log.parent_path(); mode = Env("OSFUI_CEF_AUDIT_MODE"); if (mode.empty()) mode = "baseline";
    gpuEnabled = Env("OSFUI_CEF_AUDIT_GPU") == "1";
}
inline bool Control(const std::string& text) {
    auto value = json::parse(text, nullptr, false);
    if (value.is_discarded() || value.value("name", "") != "bench.audit") return false;
    const auto& payload = value.at("payload");
    phase = payload.value("measuring", false) ? payload.value("phase", "unmeasured") : "unmeasured";
    previous.clear();
    boundaries.push_back({{"phase", phase}, {"trace_us", CefNowFromSystemTraceTime()}, {"qpc_seconds", Now()}});
    return true;
}
inline void BeginTrace() {
    if (Env("OSFUI_CEF_AUDIT_TRACE") == "1") {
        traceStarted = CefBeginTracing("toplevel,benchmark,blink,cc,viz,gpu,devtools.timeline", nullptr);
        traceDone = !traceStarted;
    }
}
class TraceEnd final : public CefEndTracingCallback {
    void OnEndTracingComplete(const CefString&) override { traceDone = true; }
    IMPLEMENT_REFCOUNTING(TraceEnd);
};
inline void EndTrace() { if (traceStarted) { traceStarted = false; if (!CefEndTracing((directory / "chromium-trace.json").wstring(), new TraceEnd)) traceDone = true; } }

struct Query {
    Microsoft::WRL::ComPtr<ID3D11Query> disjoint, start, end;
    std::string phase, name; bool pending{};
};
inline std::vector<Query> queries;
inline void PollGpu(ID3D11DeviceContext* context) {
    for (auto& q : queries) if (q.pending) {
        D3D11_QUERY_DATA_TIMESTAMP_DISJOINT frequency{}; UINT64 a{}, b{};
        if (context->GetData(q.disjoint.Get(), &frequency, sizeof(frequency), D3D11_ASYNC_GETDATA_DONOTFLUSH) != S_OK ||
            context->GetData(q.start.Get(), &a, sizeof(a), D3D11_ASYNC_GETDATA_DONOTFLUSH) != S_OK ||
            context->GetData(q.end.Get(), &b, sizeof(b), D3D11_ASYNC_GETDATA_DONOTFLUSH) != S_OK) continue;
        if (!frequency.Disjoint && frequency.Frequency) RecordFor(q.phase, q.name, double(b - a) * 1000 / frequency.Frequency);
        q.pending = false;
    }
}
struct GpuSpan {
    ID3D11DeviceContext* context{}; Query* q{};
    GpuSpan(ID3D11Device* device, ID3D11DeviceContext* ctx, const char* name) : context(ctx) {
        if (!gpuEnabled) return;
        PollGpu(ctx);
        if (queries.empty()) {
            queries.resize(32);
            for (auto& item : queries) {
                D3D11_QUERY_DESC d{D3D11_QUERY_TIMESTAMP_DISJOINT, 0};
                if (FAILED(device->CreateQuery(&d, &item.disjoint))) throw std::runtime_error("audit GPU disjoint query");
                d.Query = D3D11_QUERY_TIMESTAMP;
                if (FAILED(device->CreateQuery(&d, &item.start)) || FAILED(device->CreateQuery(&d, &item.end))) throw std::runtime_error("audit GPU timestamp query");
            }
        }
        for (auto& item : queries) if (!item.pending) { q = &item; break; }
        if (!q) { Count("gpu_query_dropped"); return; }
        q->phase = phase; q->name = name; q->pending = true;
        ctx->Begin(q->disjoint.Get()); ctx->End(q->start.Get());
    }
    void End() { if (q) { context->End(q->end.Get()); context->End(q->disjoint.Get()); q = nullptr; } }
    ~GpuSpan() { End(); }
};
inline void Write() {
    json result{{"mode", mode}, {"gpu_queries", gpuEnabled}, {"trace_complete", traceDone}, {"boundaries", boundaries}, {"phases", json::object()}};
    for (const auto& [p, entries] : metrics) for (const auto& [name, values] : entries) {
        auto sorted = values; std::sort(sorted.begin(), sorted.end()); double sum = 0; for (auto v : sorted) sum += v;
        result["phases"][p][name] = {{"count", sorted.size()}, {"sum", sum}, {"mean", sum / sorted.size()},
            {"p50", sorted[(sorted.size() - 1) / 2]}, {"p95", sorted[size_t((sorted.size() - 1) * .95)]}, {"max", sorted.back()}};
    }
    std::ofstream(directory / "audit.json") << result.dump(2);
    queries.clear();
}
}
