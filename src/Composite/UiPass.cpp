#include "Composite/UiPass.h"

#include "Composite/D3D12Compositor.h"  // RecordOverlayIntoRenderTarget
#include "Composite/UiTargetFormat.h"
#include "Core/Log.h"

#include "Composite/D3D12Prologue.h"  // GDI-free <Windows.h> + <d3d12.h>

#include "RE/C/CreationRenderer.h"
#include "RE/IDs_VTABLE.h"
#include "REL/THook.h"

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <optional>

namespace OSFUI::UiPass
{
	namespace
	{
		constexpr std::size_t kExecuteSlot = 7;

		using ExecuteHook = REL::THookVFT<void*(void*, void*, void*, void*)>;

		std::optional<ExecuteHook> g_endHook;

		std::atomic_bool g_directTargetRejectedLogged{ false };

		[[nodiscard]] bool IsUiRenderTarget(const D3D12_RESOURCE_DESC& a_desc)
		{
			return UiTargetFormat::ResolveRtv(a_desc.Format) != DXGI_FORMAT_UNKNOWN &&
				a_desc.Dimension == D3D12_RESOURCE_DIMENSION_TEXTURE2D &&
				(a_desc.Flags & D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET) &&
				a_desc.SampleDesc.Count == 1 && a_desc.DepthOrArraySize == 1 &&
				a_desc.Width >= 256 && a_desc.Height >= 256;
		}

		// After native ScaleformEnd: the UI texture is still RENDER_TARGET, before the graph transitions it.
		void RecordOverlayAfterEnd(void* a_graph)
		{
			// ScaleformBegin binds the UI layer as the HAL target; End leaves it bound.
			const auto* halTarget = RE::CreationRendererPrivate::ScaleformRenderTarget::GetCurrent();
			const auto* renderTarget = halTarget && halTarget->data ? halTarget->data->colorTarget : nullptr;
			if (!renderTarget || !renderTarget->texture || !renderTarget->texture->resource) {
				return;
			}
			auto* context = static_cast<RE::CreationRendererPrivate::GraphContext*>(a_graph)->GetOpenCommandContext();
			if (!context || !context->commandList || !context->commandPool) {
				return;
			}
			// ID 142968 only rebinds heaps for these pool flags; otherwise our descriptor heap would stay bound.
			const auto poolFlags = *(static_cast<const std::uint8_t*>(context->commandPool) + 0x20);
			if ((poolFlags & 0xFD) != 0) {
				return;
			}
			auto* list = reinterpret_cast<ID3D12GraphicsCommandList*>(context->commandList);
			auto* resource = reinterpret_cast<ID3D12Resource*>(renderTarget->texture->resource);
			if (const auto desc = resource->GetDesc(); !IsUiRenderTarget(desc)) {
				if (!g_directTargetRejectedLogged.exchange(true, std::memory_order_relaxed)) {
					REX::WARN("[UiPass] skipped ScaleformEnd target {}x{} (format {}, dimension {}, flags 0x{:X}); not a drawable UI layer",
						static_cast<std::uint64_t>(desc.Width), desc.Height, static_cast<unsigned>(desc.Format),
						static_cast<unsigned>(desc.Dimension), static_cast<unsigned>(desc.Flags));
				}
				return;
			}

			if (!RecordOverlayIntoRenderTarget(list, resource)) {
				// Early-outs happen before heaps are rebound.
				return;
			}
			context->InvalidateCachedBindingsAndBindHeaps();
		}

		void* EndThunk(void* a_this, void* a_ctx, void* a_io, void* a_r9)
		{
			void* result = (*g_endHook)(a_this, a_ctx, a_io, a_r9);
			RecordOverlayAfterEnd(a_ctx);
			return result;
		}
	}

	bool Install()
	{
		g_endHook.emplace("UiPass::ScaleformEnd", RE::VTABLE::CreationRendererPrivate____ScaleformEndRenderPass[0], kExecuteSlot, &EndThunk);
		return g_endHook->Enable();
	}
}
