#include "Composite/UiPassStateAudit.h"
#ifdef OSF_UI_PASS_DIRECT_PROBE
#include "Core/Log.h"
#include "REL/Utility.h"
#include <intrin.h>

// Fixed, observation-only D3D12 callbacks for the End.post investigation.
// Numeric list identity is compared only; no native object is retained or read later.
namespace OSFUI::UiPass::StateAudit
{
    namespace
    {
        thread_local std::uintptr_t tl_list{};
        thread_local unsigned tl_frame{}, tl_mask{}, tl_draws{}, tl_dispatches{};
        thread_local unsigned tl_gtables{}, tl_ctables{}, tl_gconstantSlot{}, tl_gconstantCount{};
        thread_local bool tl_detail{}, tl_foreign{};
        thread_local const char* tl_phase = "End.post";
        enum : unsigned { VP=1, SC=2, OM=4, GRoot=8, PSO=16, Topology=32,
            GTable=64, GConstants=128, CRoot=256, CTable=512, CConstants=1024 };
        bool Matches(ID3D12GraphicsCommandList* p) { return !tl_foreign && tl_list && reinterpret_cast<std::uintptr_t>(p) == tl_list; }
        void State(ID3D12GraphicsCommandList* p, unsigned bit) { if (Matches(p)) tl_mask |= bit; }
        void Consumer(ID3D12GraphicsCommandList* p, const char* kind, bool compute, void* caller)
        {
            if (!Matches(p)) return;
            auto& count = compute ? tl_dispatches : tl_draws;
            ++count;
            if (tl_detail && count <= 4) REX::INFO("[UiState] f={} phase={} consumer={} n={} callerRva={:X} reboundMask={:X} gTables={:X} cTables={:X} gConstants={}/{}",
                tl_frame, tl_phase, kind, count, reinterpret_cast<std::uintptr_t>(caller) - reinterpret_cast<std::uintptr_t>(GetModuleHandleW(nullptr)), tl_mask,
                tl_gtables,tl_ctables,tl_gconstantSlot,tl_gconstantCount);
        }
        using DrawFn = void(STDMETHODCALLTYPE*)(ID3D12GraphicsCommandList*, UINT, UINT, UINT, UINT);
        DrawFn draw;
        void STDMETHODCALLTYPE Draw(ID3D12GraphicsCommandList* p, UINT a, UINT b, UINT c, UINT d) { Consumer(p,"Draw",false,_ReturnAddress()); draw(p,a,b,c,d); }
        using IndexedFn = void(STDMETHODCALLTYPE*)(ID3D12GraphicsCommandList*, UINT, UINT, UINT, INT, UINT);
        IndexedFn indexed;
        void STDMETHODCALLTYPE Indexed(ID3D12GraphicsCommandList* p, UINT a, UINT b, UINT c, INT d, UINT e) { Consumer(p,"Indexed",false,_ReturnAddress()); indexed(p,a,b,c,d,e); }
        using DispatchFn = void(STDMETHODCALLTYPE*)(ID3D12GraphicsCommandList*, UINT, UINT, UINT);
        DispatchFn dispatch;
        void STDMETHODCALLTYPE Dispatch(ID3D12GraphicsCommandList* p, UINT a, UINT b, UINT c) { Consumer(p,"Dispatch",true,_ReturnAddress()); dispatch(p,a,b,c); }
        using ViewportFn = void(STDMETHODCALLTYPE*)(ID3D12GraphicsCommandList*, UINT, const D3D12_VIEWPORT*);
        ViewportFn viewport;
        void STDMETHODCALLTYPE Viewport(ID3D12GraphicsCommandList* p, UINT n, const D3D12_VIEWPORT* v) { State(p,VP); viewport(p,n,v); }
        using ScissorFn = void(STDMETHODCALLTYPE*)(ID3D12GraphicsCommandList*, UINT, const D3D12_RECT*);
        ScissorFn scissor;
        void STDMETHODCALLTYPE Scissor(ID3D12GraphicsCommandList* p, UINT n, const D3D12_RECT* v) { State(p,SC); scissor(p,n,v); }
        using OmFn = void(STDMETHODCALLTYPE*)(ID3D12GraphicsCommandList*, UINT, const D3D12_CPU_DESCRIPTOR_HANDLE*, BOOL, const D3D12_CPU_DESCRIPTOR_HANDLE*);
        OmFn om;
        void STDMETHODCALLTYPE Om(ID3D12GraphicsCommandList* p, UINT n, const D3D12_CPU_DESCRIPTOR_HANDLE* v, BOOL b, const D3D12_CPU_DESCRIPTOR_HANDLE* d) { State(p,OM); om(p,n,v,b,d); }
        using PsoFn = void(STDMETHODCALLTYPE*)(ID3D12GraphicsCommandList*, ID3D12PipelineState*);
        PsoFn pso;
        void STDMETHODCALLTYPE Pso(ID3D12GraphicsCommandList* p, ID3D12PipelineState* v) { State(p,PSO); pso(p,v); }
        using TopologyFn = void(STDMETHODCALLTYPE*)(ID3D12GraphicsCommandList*, D3D12_PRIMITIVE_TOPOLOGY);
        TopologyFn topology;
        void STDMETHODCALLTYPE TopologySet(ID3D12GraphicsCommandList* p, D3D12_PRIMITIVE_TOPOLOGY v) { State(p,Topology); topology(p,v); }
        using RootFn = void(STDMETHODCALLTYPE*)(ID3D12GraphicsCommandList*, ID3D12RootSignature*);
        RootFn groot, croot;
        void STDMETHODCALLTYPE GRootSet(ID3D12GraphicsCommandList* p, ID3D12RootSignature* v) { State(p,GRoot); groot(p,v); }
        void STDMETHODCALLTYPE CRootSet(ID3D12GraphicsCommandList* p, ID3D12RootSignature* v) { State(p,CRoot); croot(p,v); }
        using TableFn = void(STDMETHODCALLTYPE*)(ID3D12GraphicsCommandList*, UINT, D3D12_GPU_DESCRIPTOR_HANDLE);
        TableFn gtable, ctable;
        void STDMETHODCALLTYPE GTableSet(ID3D12GraphicsCommandList* p, UINT n, D3D12_GPU_DESCRIPTOR_HANDLE v) { State(p,GTable); if(Matches(p) && n<32) tl_gtables|=1u<<n; gtable(p,n,v); }
        void STDMETHODCALLTYPE CTableSet(ID3D12GraphicsCommandList* p, UINT n, D3D12_GPU_DESCRIPTOR_HANDLE v) { State(p,CTable); if(Matches(p) && n<32) tl_ctables|=1u<<n; ctable(p,n,v); }
        using ConstantsFn = void(STDMETHODCALLTYPE*)(ID3D12GraphicsCommandList*, UINT, UINT, const void*, UINT);
        ConstantsFn gconstants, cconstants;
        void STDMETHODCALLTYPE GConstantsSet(ID3D12GraphicsCommandList* p, UINT r, UINT n, const void* v, UINT o) { State(p,GConstants); if(Matches(p)){tl_gconstantSlot=r; tl_gconstantCount=n;} gconstants(p,r,n,v,o); }
        void STDMETHODCALLTYPE CConstantsSet(ID3D12GraphicsCommandList* p, UINT r, UINT n, const void* v, UINT o) { State(p,CConstants); cconstants(p,r,n,v,o); }
        using CloseFn = HRESULT(STDMETHODCALLTYPE*)(ID3D12GraphicsCommandList*);
        CloseFn close;
        HRESULT STDMETHODCALLTYPE Close(ID3D12GraphicsCommandList* p)
        {
            if (Matches(p)) {
                if (tl_detail) REX::INFO("[UiState] f={} CLOSE draws={} dispatches={} reboundMask={:X}", tl_frame,tl_draws,tl_dispatches,tl_mask);
                tl_list=0;
            }
            return close(p);
        }
    }
    bool Install(void** t)
    {
        // Slots are from the local d3d12.h ID3D12GraphicsCommandList interface.
        close=reinterpret_cast<CloseFn>(t[9]); draw=reinterpret_cast<DrawFn>(t[12]); indexed=reinterpret_cast<IndexedFn>(t[13]); dispatch=reinterpret_cast<DispatchFn>(t[14]);
        topology=reinterpret_cast<TopologyFn>(t[20]); viewport=reinterpret_cast<ViewportFn>(t[21]); scissor=reinterpret_cast<ScissorFn>(t[22]); pso=reinterpret_cast<PsoFn>(t[25]);
        croot=reinterpret_cast<RootFn>(t[29]); groot=reinterpret_cast<RootFn>(t[30]); ctable=reinterpret_cast<TableFn>(t[31]); gtable=reinterpret_cast<TableFn>(t[32]);
        cconstants=reinterpret_cast<ConstantsFn>(t[35]); gconstants=reinterpret_cast<ConstantsFn>(t[36]); om=reinterpret_cast<OmFn>(t[46]);
        const unsigned slots[]{9,12,13,14,20,21,22,25,29,30,31,32,35,36,46};
        void* const thunks[]{reinterpret_cast<void*>(&Close),reinterpret_cast<void*>(&Draw),reinterpret_cast<void*>(&Indexed),reinterpret_cast<void*>(&Dispatch),
            reinterpret_cast<void*>(&TopologySet),reinterpret_cast<void*>(&Viewport),reinterpret_cast<void*>(&Scissor),reinterpret_cast<void*>(&Pso),
            reinterpret_cast<void*>(&CRootSet),reinterpret_cast<void*>(&GRootSet),reinterpret_cast<void*>(&CTableSet),reinterpret_cast<void*>(&GTableSet),
            reinterpret_cast<void*>(&CConstantsSet),reinterpret_cast<void*>(&GConstantsSet),reinterpret_cast<void*>(&Om)};
        void* originals[15]{};
        for (unsigned i=0;i<15;++i) {
            originals[i]=t[slots[i]];
            if (!REL::WriteSafeData(&t[slots[i]],thunks[i])) {
                for (unsigned j=0;j<i;++j) REL::WriteSafeData(&t[slots[j]],originals[j]);
                REX::ERROR("[UiState] audit install failed slot={}",slots[i]);
                return false;
            }
        }
        REX::INFO("[UiState] observation callbacks installed; mask VP=1 SC=2 OM=4 GRoot=8 PSO=10 Topology=20 GTable=40 GConstants=80 CRoot=100 CTable=200 CConstants=400 (hex)");
        return true;
    }
    void Begin() { tl_list=0; }
    void End(std::uintptr_t list, unsigned frame, bool detail) { tl_list=list; tl_frame=frame; tl_detail=detail; tl_mask=tl_draws=tl_dispatches=tl_gtables=tl_ctables=tl_gconstantSlot=tl_gconstantCount=0; tl_phase="End.post"; }
    void Phase(const char* name) { tl_phase=name; }
    void Foreign(bool active) { tl_foreign=active; }
}
#endif
