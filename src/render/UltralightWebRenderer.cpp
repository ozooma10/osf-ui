#include "render/UltralightWebRenderer.h"

#if defined(SWUI_WITH_ULTRALIGHT)

// NOTE: deliberately not including <Ultralight/Ultralight.h> yet. This stub
// proves the build wiring (option, SDK include/lib paths) without committing
// to an SDK surface. When Phase 1 starts, includes go here.

namespace SWUI
{
	// TODO(phase1): real Ultralight integration. Required work, in order:
	//  - custom ultralight::FileSystem that only serves files from the active
	//    view's rootDir (enforce ViewPermissions::filesystem; no absolute
	//    paths, no '..' traversal)
	//  - font loading: ship/locate a default font, configure FontLoader; the
	//    default Win32 font loader may be acceptable, verify in-game
	//  - clipboard policy: provide a null clipboard by default (no game ->
	//    web clipboard leaks); revisit when text entry lands (Phase 4)
	//  - JS bridge wiring: expose exactly one function
	//    (window.starfield.postMessage(json)) and one callback; route through
	//    MessageBridge, never expose arbitrary native calls
	//  - renderer update cadence: ultralight::Renderer::Update()+Render() must
	//    be driven from a known-safe thread at a known cadence; blocked on the
	//    game tick question in docs/reverse-engineering-notes.md
	//  - device/surface integration: start with the CPU surface
	//    (ultralight::BitmapSurface) and hand pixels to ICompositor; GPU
	//    driver integration is explicitly out of scope until Phase 3
	//  - network must stay disabled (view permissions force it off)

	bool UltralightWebRenderer::Initialize(const RendererConfig& a_config)
	{
		_config = a_config;
		REX::WARN("UltralightWebRenderer: built with SWUI_WITH_ULTRALIGHT but the backend is a stub; "
				  "no SDK initialization is performed yet (see docs/renderer-plan.md Phase 1)");
		return true;
	}

	void UltralightWebRenderer::Shutdown() {}

	void UltralightWebRenderer::LoadView(const ViewManifest& a_manifest)
	{
		REX::WARN("UltralightWebRenderer: LoadView('{}') ignored — stub backend", a_manifest.id);
	}

	void UltralightWebRenderer::Resize(std::uint32_t, std::uint32_t) {}

	void UltralightWebRenderer::Update(double) {}

	std::optional<FrameBufferView> UltralightWebRenderer::Render()
	{
		return std::nullopt;
	}

	void UltralightWebRenderer::SendMessageToWeb(std::string_view) {}
}

#endif  // SWUI_WITH_ULTRALIGHT
