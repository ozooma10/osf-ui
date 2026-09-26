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
		bool g_installed = false;

		std::atomic_bool g_directTargetRejectedLogged{ false };

		[[nodiscard]] bool IsUiRenderTarget(const D3D12_RESOURCE_DESC& a_desc)
		{
			return UiTargetFormat::ResolveRtv(a_desc.Format) != DXGI_FORMAT_UNKNOWN &&
				a_desc.Dimension == D3D12_RESOURCE_DIMENSION_TEXTURE2D &&
				(a_desc.Flags & D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET) &&
				a_desc.SampleDesc.Count == 1 && a_desc.DepthOrArraySize == 1 &&
				a_desc.Width >= 256 && a_desc.Height >= 256;
		}

		// Runs after the native ScaleformEnd returns, while the UI texture is still in RENDER_TARGET and before the graph transitions it for its consumer.
		void RecordOverlayAfterEnd(void* a_graph)
		{
			// ScaleformBegin bound the UI layer as the HAL's target and End leaves it in place.
			const auto* halTarget = RE::CreationRendererPrivate::ScaleformRenderTarget::GetCurrent();
			const auto* renderTarget = halTarget && halTarget->data ? halTarget->data->colorTarget : nullptr;
			if (!renderTarget || !renderTarget->texture || !renderTarget->texture->resource) {
				return;
			}
			auto* context = static_cast<RE::CreationRendererPrivate::GraphContext*>(a_graph)->GetOpenCommandContext();
			if (!context || !context->commandList || !context->commandPool) {
				return;
			}
			// ID 142968 only rebinds heaps for these command-pool flags. Skip before touching the list if its reset would leave our descriptor heap bound.
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
				// Early-outs occur before the compositor rebinds descriptor heaps.
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
		if (g_installed) {
			return true;
		}

		g_endHook.emplace("UiPass::ScaleformEnd", RE::VTABLE::CreationRendererPrivate____ScaleformEndRenderPass[0], kExecuteSlot, &EndThunk);
		g_endHook->Enable();
		g_installed = true;
		return true;
	}
}
