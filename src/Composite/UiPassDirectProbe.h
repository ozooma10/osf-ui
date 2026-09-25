#pragma once

// Investigation only. Enable with xmake f --ui_pass_direct_probe=y.
// No graph object or IO-table pointer may escape Enter/Leave.
#ifdef OSF_UI_PASS_DIRECT_PROBE
#include "Composite/D3D12Prologue.h"

namespace OSFUI::UiPass::DirectProbe
{
    enum class Pass { Begin, End, Composite };
    void Enter(Pass, void* graph, void* io);
    void Leave(Pass, void* graph, void* io, ID3D12GraphicsCommandList* observedList,
        UINT heapCount, ID3D12DescriptorHeap* const* heaps);
    void Barrier(ID3D12GraphicsCommandList*, UINT, const D3D12_RESOURCE_BARRIER*);
    void Candidate(ID3D12GraphicsCommandList*, ID3D12Resource*, bool fg, bool first);
    void DrawResult(bool drew);
}
#endif
