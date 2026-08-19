#pragma once

namespace OSFUI
{
	struct FrameBufferView
	{
		std::uint32_t width{ 0 };
		std::uint32_t height{ 0 };
		std::uint64_t ringGeneration{ 0 };
		std::uint64_t frameIndex{ 0 };
		std::uint32_t sharedSlot{ 0 };
	};

	struct SharedRingDesc
	{
		static constexpr std::size_t kMaxSlots = 8;

		void*         slotHandles[kMaxSlots]{};
		std::uint32_t slotCount{ 0 };
		void*         produceFence{ nullptr };
		void*         consumeFence{ nullptr };
		std::uint32_t width{ 0 };
		std::uint32_t height{ 0 };
		std::uint32_t adapterLuidLow{ 0 };
		std::uint32_t adapterLuidHigh{ 0 };
		std::uint64_t generation{ 0 };
	};
}
