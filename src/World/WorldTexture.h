#pragma once

#include <cstddef>
#include <cstdint>
#include <span>
#include <string>
#include <vector>

#include "Render/SharedTextureTransport.h"

namespace OSFUI::WorldTexture
{
	inline constexpr std::uint64_t kDefaultBudgetBytes = 256ull * 1024 * 1024;

	struct SurfaceDesc
	{
		std::string   id;
		std::string   texture;
		std::uint32_t width{ 0 };
		std::uint32_t height{ 0 };
	};

	struct SurfaceStats
	{
		std::string   id;
		std::string   texture;
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
		std::uint64_t outputGeneration{ 0 };
		std::uint64_t residentBytes{ 0 };
		std::uint64_t allocationDeferrals{ 0 };
		std::uint32_t engineOwners{ 0 };
		bool          evictionPending{ false };
		bool          gpuFailed{ false };
	};

	// Configure before Install. Indices stay identical to the supplied span;
	// duplicate IDs/asset keys or invalid entries reject the entire configuration.
	bool Configure(std::span<const SurfaceDesc> a_surfaces, std::uint64_t budgetBytes = kDefaultBudgetBytes);
	struct CacheStats
	{
		std::uint64_t budgetBytes, residentBytes, retiredOutputs, allocationDeferrals;
	};
	[[nodiscard]] CacheStats SnapshotCache();
	// Defers until the engine has destroyed every owning resource and all our
	// commands referencing this output have completed. Never rewrites a live SRV.
	void RequestEviction(std::size_t index);
	// Call once the renderer exists, before the placeholder material is loaded.
	// Hooks last for the process; outputs have engine and GPU ownership leases.
	bool               Install();
	[[nodiscard]] bool IsInstalled();

	// Transfers ownership of every duplicated handle, including on failure.
	void SetSharedRing(std::size_t a_surface, const SharedRingDesc& a_desc);
	// Publishes pending work. GPU copies run on the next engine DIRECT submission.
	// The caller must not separately acknowledge these frames to the producer.
	void                                    Submit(std::size_t a_surface, const FrameBufferView& a_frame);
	[[nodiscard]] std::vector<SurfaceStats> Snapshot();
}
