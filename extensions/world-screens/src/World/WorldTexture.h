#pragma once

#include <cstddef>
#include <cstdint>
#include <span>
#include <string>
#include <vector>

#include "OSFUI_Surfaces.h"
#include "World/SurfaceCatalog.h"

namespace OSFUI::WorldTexture
{
	inline constexpr std::uint64_t kDefaultBudgetBytes = 256ull * 1024 * 1024;

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

    void SetService(API::Surfaces::IService*);
    // Runtime tick reports whether any reference binds this view. Copies run only while the engine
    // also holds the material; the lease (and its browser) is returned after a short grace without it.
    // Rendering stops before returning a released lease.
    void SetDemand(std::size_t index, bool bound);
    API::Surfaces::Handle Feed(std::size_t index);
	[[nodiscard]] std::vector<SurfaceStats> Snapshot();
}
