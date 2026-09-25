#include "Composite/UiPassDirectProbe.h"
#include "Composite/UiPassStateAudit.h"

#ifdef OSF_UI_PASS_DIRECT_PROBE
#include "API/BridgeApi.h"
#include "Core/Log.h"
#include "REL/Relocation.h"
#include <atomic>
#include <cstring>

namespace OSFUI::UiPass::DirectProbe
{
    namespace
    {
        constexpr unsigned kFrames = 2400;
        std::atomic_uint g_frames{0}, g_candidates{0}, g_listMatches{0}, g_targetMatches{0};
        std::atomic_uint g_heapSamples{0}, g_heapMatches{0}, g_draws{0};
        std::atomic_uint g_endMatches{0}, g_endTransitions{0}, g_insideEndTransitions{0};
        std::atomic_uint g_copiedHandleSamples{0}, g_copiedHandleMatches{0};
        // Copied numeric identities only (no COM ownership); never dereference these after the pass returns.
        struct Snapshot { std::uintptr_t list{}, resource[2]{}, heap[2]{}, copiedResource{}; };
        thread_local Snapshot tl_begin{}, tl_last{};
        thread_local unsigned tl_frame{}, tl_barriers{};
        thread_local const char* tl_phase = "outside";
        thread_local bool tl_active{}, tl_detail{};

        bool ReadBytes(std::uintptr_t p, void* out, std::size_t size)
        {
            if (!p) return false;
            __try { std::memcpy(out, reinterpret_cast<const void*>(p), size); return true; }
            __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
        }
        template<class T = std::uintptr_t> T Read(std::uintptr_t p)
        {
            T value{};
            ReadBytes(p, &value, sizeof(value));
            return value;
        }
        struct Handle { std::uint32_t words[3]; };
        thread_local Handle tl_beginHandle{};
        thread_local bool tl_haveBeginHandle{};
        using ResolveFn = std::uintptr_t (*)(void*, const Handle*);
        std::uintptr_t TryResolve(ResolveFn fn, std::uintptr_t pool, const Handle* handle)
        {
            __try { return fn(reinterpret_cast<void*>(pool), handle); }
            __except (EXCEPTION_EXECUTE_HANDLER) { return 0; }
        }
        std::uintptr_t Resolve(std::uintptr_t pool, const Handle* handle)
        {
            static REL::Relocation<ResolveFn> fn{REL::ID(145523)};
            return TryResolve(fn.get(), pool, handle);
        }
        std::uintptr_t GetContext(std::uintptr_t graph)
        {
            using Fn = std::uintptr_t (*)(void*);
            static REL::Relocation<Fn> fn{REL::ID(144161)};
            // Do not initialize a context the native pass did not use.
            if (!Read(Read(graph + 0x138) + 0x120)) return 0;
            return fn(reinterpret_cast<void*>(graph));
        }
        const char* Name(Pass pass, bool after)
        {
            if (pass == Pass::Begin) return after ? "Begin.post" : "Begin.pre";
            if (pass == Pass::End) return after ? "End.post" : "End.pre";
            return after ? "Composite.post" : "Composite.pre";
        }
        Snapshot Capture(Pass pass, bool after, void* graphArg, void* ioArg)
        {
            Snapshot result{};
            const auto graph = reinterpret_cast<std::uintptr_t>(graphArg);
            const auto io = reinterpret_cast<std::uintptr_t>(ioArg);
            const auto batch = Read(graph + 0x138);
            const auto cached = Read(batch + 0x120);
            const auto ctx = after ? GetContext(graph) : cached;
            result.list = Read(ctx + 0x60);
            const auto device = Read(ctx + 0x30);
            result.heap[0] = Read(Read(device + 0x470) + 0x1D0);
            result.heap[1] = Read(Read(device + 0x468) + 0x1D0);
            const auto array = Read(io);
            const auto pool = Read(io + 8);
            const auto storage = Read<std::int32_t>(array + 8) < 0 ? array + 0x10 : Read(array + 0x10);
            // Native Begin/Composite use +0/+0x20. Run 1 confirmed array+0
            // is the count (Begin=3, End=1, Composite=2); resolve End's sole entry.
            const auto count = Read<unsigned>(array);
            if (count <= 8 && storage && pool && Read<unsigned>(pool + 8) < 4096) {
                for (unsigned i = 0; i != (std::min)(count, 2u); ++i) {
                    Handle handle{};
                    if (!ReadBytes(storage + i * 0x20, &handle, sizeof(handle))) continue;
                    if (pass == Pass::Begin && after && i == 0) {
                        tl_beginHandle = handle;
                        tl_haveBeginHandle = true;
                    }
                    const auto rt = Resolve(pool, &handle);
                    const auto texture = Read(rt + 0x58);
                    result.resource[i] = Read(texture + 0x38);
                    if (tl_detail) {
                        REX::INFO("[UiDirect] f={} {} io[{}] handle={:X}/{:X}/{:X} rt={:X} texture={:X} resource={:X} rtv={:X} wh={}/{}",
                            tl_frame, Name(pass, after), i, handle.words[0], handle.words[1], handle.words[2], rt, texture,
                            result.resource[i], Read(Read(rt + 8)), Read<unsigned>(rt + 0x14), Read<unsigned>(rt + 0x18));
                    }
                }
            }
            if (pass == Pass::End && tl_haveBeginHandle && pool && Read<unsigned>(pool + 8) < 4096) {
                const auto rt = Resolve(pool, &tl_beginHandle);
                result.copiedResource = Read(Read(rt + 0x58) + 0x38);
                if (after) {
                    ++g_copiedHandleSamples;
                    if (result.copiedResource && result.copiedResource == tl_begin.resource[0]) ++g_copiedHandleMatches;
                }
                if (tl_detail) REX::INFO("[UiDirect] f={} {} copiedBeginHandle resource={:X} beginMatch={} rtv={:X} wh={}/{}",
                    tl_frame, Name(pass, after), result.copiedResource, result.copiedResource == tl_begin.resource[0],
                    Read(Read(rt + 8)), Read<unsigned>(rt + 0x14), Read<unsigned>(rt + 0x18));
            }
            if (tl_detail) {
                const auto layout = Read(ctx);
                const auto definition = Read(layout);
                REX::INFO("[UiDirect] f={} {} layoutKind={} groups={} constantsBytes={} constantsSlot={}", tl_frame,Name(pass,after),
                    Read<unsigned char>(definition+4), Read<unsigned>(layout+0x40),Read<unsigned char>(definition+6),Read<unsigned>(layout+0x58));
                for (unsigned j=0; layout && j<(std::min)(Read<unsigned>(layout+0x40),4u); ++j) {
                    const auto group = Read(ctx+0x10+j*8);
                    const auto desc = Read(group);
                    REX::INFO("[UiDirect] f={} {} group{} present={} rootSlot={} tables={}/{}",tl_frame,Name(pass,after),j,group!=0,
                        Read<unsigned>(layout+0x48+j*4),Read<unsigned>(desc+0x21C),Read<unsigned>(desc+0x218));
                }
                const auto index = Read<unsigned>(graph + 0x140);
                const auto node = index < 6 ? Read(graph + 0x108 + index * 8) : 0;
                REX::INFO("[UiDirect] f={} {} graph={:X} batch={:X} ctx={:X} cached={:X} list={:X} heaps={:X}/{:X} arrayHead={:X}/{:X}/{:X} poolCount={} node={:X} rtInfo={:X} cache={:X}/{:X}/{:X}/{:X}/{:X}/{:X}",
                    tl_frame, Name(pass, after), graph, batch, ctx, cached, result.list, result.heap[0], result.heap[1],
                    Read<unsigned>(array), Read<unsigned>(array+4), Read<unsigned>(array+8), Read<unsigned>(pool+8), node, Read(node+0x28),
                    Read(ctx), Read(ctx+8), Read(ctx+0x10), Read(ctx+0x18), Read(ctx+0x20), Read(ctx+0x28));
            }
            return result;
        }
    }

    void Enter(Pass pass, void* graph, void* io)
    {
        static const bool supported = [] {
            const auto base = reinterpret_cast<std::uintptr_t>(GetModuleHandleW(nullptr));
            const bool ok = REL::ID(144161).address() == base + 0x2A7D600 &&
                REL::ID(145523).address() == base + 0x2B15480 &&
                REL::ID(145955).address() == base + 0x2B6C330 &&
                REL::ID(145956).address() == base + 0x2B6C520 &&
                REL::ID(145827).address() == base + 0x2B570F0;
            REX::INFO("[UiDirect] anchorGate={} budget={} frames; observation only", ok, kFrames);
            return ok;
        }();
        if (!supported) return;
        if (pass == Pass::Begin) {
            StateAudit::Begin();
            // Explicit diagnostic controls use the public queued UI API. This bypasses
            // the example's rejected settings schema, not the compositor draw path.
            static HANDLE open = CreateEventW(nullptr, FALSE, FALSE, L"Local\\OSFUI.UiPassDirectProbe.Open");
            static HANDLE close = CreateEventW(nullptr, FALSE, FALSE, L"Local\\OSFUI.UiPassDirectProbe.Close");
            if (open && WaitForSingleObject(open, 0) == WAIT_OBJECT_0)
                REX::INFO("[UiDirect] panel open queued={}", API::BridgeApi::Get().RequestMenu("osfui-example/panel", true));
            if (close && WaitForSingleObject(close, 0) == WAIT_OBJECT_0)
                REX::INFO("[UiDirect] panel close queued={}", API::BridgeApi::Get().RequestMenu("osfui-example/panel", false));
            static HANDLE arm = CreateEventW(nullptr, FALSE, FALSE, L"Local\\OSFUI.UiPassDirectProbe.Arm");
            if (arm && WaitForSingleObject(arm, 0) == WAIT_OBJECT_0) {
                g_frames = 0; g_candidates = 0; g_listMatches = 0; g_targetMatches = 0;
                g_heapSamples = 0; g_heapMatches = 0; g_draws = 0;
                g_endMatches = 0; g_endTransitions = 0; g_insideEndTransitions = 0;
                g_copiedHandleSamples = 0; g_copiedHandleMatches = 0;
                REX::INFO("[UiDirect] REARM observation only");
            }
            const auto frame = g_frames.fetch_add(1) + 1;
            tl_frame = frame;
            tl_active = frame <= kFrames;
            tl_detail = frame <= 4 || frame % 120 == 0;
            tl_begin = {};
            tl_haveBeginHandle = false;
            tl_barriers = 0;
            if (frame == kFrames + 1) {
                REX::INFO("[UiDirect] COMPLETE frames={} candidates={} listMatches={} targetMatches={} heapSamples={} heapMatches={} actualOverlayDraws={} endIOMatches={} postEndTransitions={} insideEndTransitions={} copiedHandleSamples={} copiedHandleMatches={}",
                    kFrames, g_candidates.load(), g_listMatches.load(), g_targetMatches.load(),
                    g_heapSamples.load(), g_heapMatches.load(), g_draws.load(), g_endMatches.load(),
                    g_endTransitions.load(), g_insideEndTransitions.load(), g_copiedHandleSamples.load(), g_copiedHandleMatches.load());
            }
        }
        if (!tl_active) return;
        tl_phase = Name(pass, false);
        StateAudit::Phase(tl_phase);
        tl_last = Capture(pass, false, graph, io);
    }
    void Leave(Pass pass, void* graph, void* io, ID3D12GraphicsCommandList* observedList,
        UINT heapCount, ID3D12DescriptorHeap* const* heaps)
    {
        if (!tl_active) return;
        tl_phase = Name(pass, true);
        tl_last = Capture(pass, true, graph, io);
        StateAudit::Phase(tl_phase);
        if (pass == Pass::End) StateAudit::End(tl_last.list, tl_frame, tl_detail);
        if (pass == Pass::Begin) tl_begin = tl_last;
        if (tl_last.list && reinterpret_cast<std::uintptr_t>(observedList) == tl_last.list && heapCount == 2) {
            ++g_heapSamples;
            const bool match = tl_last.heap[0] == reinterpret_cast<std::uintptr_t>(heaps[0]) &&
                tl_last.heap[1] == reinterpret_cast<std::uintptr_t>(heaps[1]);
            if (match) ++g_heapMatches;
            if (!match || tl_detail) REX::INFO("[UiDirect] f={} {} heapMatch={} observed={:X}/{:X}", tl_frame, tl_phase, match,
                reinterpret_cast<std::uintptr_t>(heaps[0]), reinterpret_cast<std::uintptr_t>(heaps[1]));
        }
    }
    void Barrier(ID3D12GraphicsCommandList* list, UINT count, const D3D12_RESOURCE_BARRIER* barriers)
    {
        if (!tl_active || !barriers) return;
        ++tl_barriers;
        for (UINT i = 0; i != count; ++i) {
            const auto& b = barriers[i];
            if (b.Type != D3D12_RESOURCE_BARRIER_TYPE_TRANSITION) continue;
            const auto res = reinterpret_cast<std::uintptr_t>(b.Transition.pResource);
            if (res != tl_begin.resource[0] && res != tl_begin.resource[1] &&
                res != tl_last.resource[0] && res != tl_last.resource[1]) continue;
            if (b.Transition.StateBefore == D3D12_RESOURCE_STATE_RENDER_TARGET && res == tl_last.copiedResource) {
                if (std::strcmp(tl_phase, "End.post") == 0) ++g_endTransitions;
                if (std::strcmp(tl_phase, "End.pre") == 0) ++g_insideEndTransitions;
            }
            if (!tl_detail || tl_barriers > 24) continue;
            REX::INFO("[UiDirect] f={} {} barrier#{} list={:X} res={:X} states={:X}->{:X} flags={} sub={:X}",
                tl_frame, tl_phase, tl_barriers, reinterpret_cast<std::uintptr_t>(list), res,
                static_cast<unsigned>(b.Transition.StateBefore), static_cast<unsigned>(b.Transition.StateAfter),
                static_cast<unsigned>(b.Flags), b.Transition.Subresource);
        }
    }
    void Candidate(ID3D12GraphicsCommandList* list, ID3D12Resource* resource, bool fg, bool first)
    {
        if (!tl_active) return;
        ++g_candidates;
        const auto l = reinterpret_cast<std::uintptr_t>(list), r = reinterpret_cast<std::uintptr_t>(resource);
        const bool listMatch = l == tl_last.list;
        const bool targetMatch = r == tl_begin.resource[0] || r == tl_begin.resource[1] || r == tl_last.resource[0] || r == tl_last.resource[1];
        if (listMatch) ++g_listMatches;
        if (targetMatch) ++g_targetMatches;
        if (std::strcmp(tl_phase, "End.post") == 0 && r == tl_last.copiedResource) ++g_endMatches;
        if (tl_detail || !listMatch || !targetMatch) {
            const auto d = resource->GetDesc();
            REX::INFO("[UiDirect] f={} {} CANDIDATE list={:X} res={:X} listMatch={} targetMatch={} beginSlot={}/{} lastSlot={}/{} fg={} first={} desc={}x{} fmt={}",
                tl_frame, tl_phase, l, r, listMatch, targetMatch, r==tl_begin.resource[0], r==tl_begin.resource[1],
                r==tl_last.resource[0], r==tl_last.resource[1], fg, first, d.Width, d.Height, static_cast<unsigned>(d.Format));
        }
    }
    void DrawResult(bool drew)
    {
        if (tl_active && drew) {
            const auto count = ++g_draws;
            if (count == 1 || count % 120 == 0)
                REX::INFO("[UiDirect] REFERENCE actualOverlayDraws={} f={} phase={}", count, tl_frame, tl_phase);
        }
    }
}
#endif
