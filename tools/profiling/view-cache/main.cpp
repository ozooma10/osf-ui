#include "Views/ViewCache.h"
#include <Windows.h>
#include <chrono>
#include <fstream>
#include <iostream>
#include <stdexcept>

namespace fs = std::filesystem;
using Clock = std::chrono::steady_clock;
static std::uint64_t identityCalls;
static double identityMs;

bool CacheBenchEquivalent(const fs::path& a, const fs::path& b, std::error_code& ec)
{
    const auto start = Clock::now();
    const bool result = fs::equivalent(a, b, ec);
    identityMs += std::chrono::duration<double, std::milli>(Clock::now() - start).count();
    ++identityCalls;
    return result;
}

void Write(const fs::path& path, const std::string& text)
{
    fs::create_directories(path.parent_path());
    std::ofstream out(path, std::ios::binary);
    out << text;
    out.close();
    if (!out) throw std::runtime_error("Fixture write failed");
}

int wmain(int argc, wchar_t** argv)
{
    try {
        if (argc != 2 && argc != 3) throw std::runtime_error("Usage: benchmark NEW_OUTPUT_DIR [READ_ONLY_SOURCE_TREE]");
        const auto root = fs::absolute(argv[1]);
        if (!fs::create_directory(root)) throw std::runtime_error("Output must be new; existing evidence is never removed");
        std::ofstream csv(root / "samples.csv");
        csv << "profiled,entries,phase,repeat,elapsed_ms,identity_calls,identity_ms,read_ops,write_ops,other_ops,read_bytes,write_bytes,reused,removed,failed\n";
        const auto run = [&](const fs::path& source, const fs::path& cache, int entries, const char* phase, int repeat) {
            identityCalls = 0; identityMs = 0;
            IO_COUNTERS before{}, after{};
            if (!GetProcessIoCounters(GetCurrentProcess(), &before)) throw std::runtime_error("I/O counters unavailable");
            const auto start = Clock::now();
            std::string error;
            const auto result = OSFUI::ViewCache::PrepareMods(source, cache, "bench-v1", "bench", error);
            const double elapsed = std::chrono::duration<double, std::milli>(Clock::now() - start).count();
            if (!GetProcessIoCounters(GetCurrentProcess(), &after)) throw std::runtime_error("I/O counters unavailable");
            if (!result) {
                std::ofstream failures(root / "failures.txt", std::ios::app);
                failures << entries << ' ' << phase << ' ' << repeat << ": " << elapsed << " ms: " << error << '\n';
                std::cerr << entries << ' ' << phase << ' ' << repeat << ": " << error << '\n';
                return false;
            }
            std::size_t reused = 0;
            for (const auto& entry : result->entries) reused += entry.cache.reused;
            if (result->entries.size() != entries || result->failed ||
                (std::string_view(phase) == "warm" && reused != entries))
                throw std::runtime_error("Cache workload did not execute as expected");
            csv << CACHE_BENCH_PROFILE << ',' << entries << ',' << phase << ',' << repeat << ',' << elapsed << ',' << identityCalls << ',' << identityMs << ','
                << after.ReadOperationCount-before.ReadOperationCount << ',' << after.WriteOperationCount-before.WriteOperationCount << ','
                << after.OtherOperationCount-before.OtherOperationCount << ',' << after.ReadTransferCount-before.ReadTransferCount << ','
                << after.WriteTransferCount-before.WriteTransferCount << ',' << reused << ',' << result->removed << ',' << result->failed << '\n';
            csv.flush();
            std::cout << entries << ' ' << phase << ' ' << repeat << ": " << elapsed << " ms, " << identityCalls << " identity calls\n";
            return true;
        };
        // Retain failures. Only initial preparation is retried, with its own label;
        // a partially populated retry must never be reported as an empty-cache sample.
        const auto prepare = [&](const fs::path& source, const fs::path& cache, int entries, const char* initial = "empty-cache") {
            for (int attempt = 0; attempt < 64; ++attempt) {
                if (run(source, cache, entries, attempt ? "partial-retry" : initial, attempt)) return;
                Sleep(250);
            }
            throw std::runtime_error("Initial cache preparation failed 64 times; see failures.txt");
        };
        if (argc == 3) {
            // Real payload is only read. Cache and all writes stay under the new evidence directory.
            const auto source = fs::absolute(argv[2]);
            int entries = 0;
            for (const auto& ignored : fs::directory_iterator(source)) ++entries;
            prepare(source, root / "cache", entries);
            for (int r = 1; r <= 7; ++r) if (!run(source, root / "cache", entries, "warm", r)) throw std::runtime_error("Warm trial failed");
        } else for (int entries : {1, 8, 32, 128}) {
            const auto fixture = root / std::to_string(entries);
            const auto source = fixture / "source", cache = fixture / "cache";
            for (int i = 0; i < entries; ++i) {
                const auto mod = source / ("mod" + std::to_string(i));
                Write(mod / "main/index.html", "<html><body>Cache performance fixture</body></html>");
                Write(mod / "main/manifest.json", R"({"manifestVersion":1,"kind":"menu"})");
                Write(mod / "assets/payload.bin", std::string(65536, 'a' + i % 26));
            }
            prepare(source, cache, entries);
            for (int r = 1; r <= 7; ++r) if (!run(source, cache, entries, "warm", r)) throw std::runtime_error("Warm trial failed");
            Write(source / "mod0/main/index.html", "<html><body>Updated cache fixture content</body></html>");
            if (!run(source, cache, entries, "one-changed", 0)) prepare(source, cache, entries, "partial-retry");
            Write(cache / "mods/removed-mod/stale.txt", "stale");
            if (!run(source, cache, entries, "one-stale", 0)) throw std::runtime_error("Stale cleanup trial failed");
            if (fs::exists(cache / "mods/removed-mod")) throw std::runtime_error("Stale cache not removed");
        }
        return 0;
    } catch (const std::exception& e) {
        std::cerr << e.what() << '\n';
        return 1;
    }
}
