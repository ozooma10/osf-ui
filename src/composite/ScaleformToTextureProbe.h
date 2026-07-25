#pragma once

namespace OSFUI::ScaleformToTextureProbe
{
	// Dev-mode-only characterization hook for Starfield's native
	// ScaleformToTextureRenderPass. The hook is observational: it forwards the
	// pass unchanged and emits a bounded log sample containing the pass's three
	// tail resource identifiers and render-worker arguments.
	//
	// Installation is fail-closed against the exact Address Library vtable and
	// implementation IDs proven on 1.16.244. There is no uninstall; process exit
	// owns teardown, like the other engine vtable hooks.
	bool Install();
}
