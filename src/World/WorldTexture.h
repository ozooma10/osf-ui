#pragma once

#include <cstddef>
#include <cstdint>
#include <span>
#include <string>
#include <vector>

#include "Render/SharedTextureTransport.h"

namespace OSFUI::WorldTexture
{
	inline constexpr std::size_t kMaxSurfaces = 4;

	struct SurfaceDesc
	{
		std::string id;
		std::uint32_t placeholderSize{ 0 };
		std::uint32_t width{ 0 };
		std::uint32_t height{ 0 };
	};

	struct SurfaceStats
	{
		std::string id;
		std::uint32_t placeholderSize{ 0 };
		std::uint32_t width{ 0 };
		std::uint32_t height{ 0 };
		std::uint64_t boundDescriptors{ 0 };
		std::uint64_t submittedFrames{ 0 };
		std::uint64_t copiedFrames{ 0 };
		std::uint64_t completedFrames{ 0 };
		std::uint64_t skippedBusy{ 0 };
		std::uint64_t rejectedFrames{ 0 };
		std::uint64_t ringOpenFailures{ 0 };
		std::uint64_t producerDisconnects{ 0 };
		std::uint64_t ringGeneration{ 0 };
		// Frame counts are cumulative; last-frame serials refer to ringGeneration.
		std::uint64_t lastCopiedFrame{ 0 };
		std::uint64_t lastCompletedFrame{ 0 };
		bool gpuFailed{ false };
	};

	// Configure before Install. Indices stay identical to the supplied span;
	// duplicate IDs/signatures or invalid entries reject the entire configuration.
	bool Configure(std::span<const SurfaceDesc> a_surfaces);
	// Call once the renderer exists, before the placeholder material is loaded.
	// Successful installation and destination textures last for the process.
	bool Install();

	// Transfers ownership of every duplicated handle, including on failure.
	void SetSharedRing(std::size_t a_surface, const SharedRingDesc& a_desc);
	// Publishes pending work. GPU copies run on the next engine DIRECT submission.
	// The caller must not separately acknowledge these frames to the producer.
	void Submit(std::size_t a_surface, const FrameBufferView& a_frame);
	[[nodiscard]] std::vector<SurfaceStats> Snapshot();
}
