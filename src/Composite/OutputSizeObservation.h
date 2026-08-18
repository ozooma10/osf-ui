#pragma once

#include <atomic>
#include <cstdint>
#include <optional>

namespace OSFUI
{
	struct OutputSize
	{
		std::uint32_t width{ 0 };
		std::uint32_t height{ 0 };
	};

	// Render workers publish the latest engine UI target size; the runtime reads it on the main thread.
	class OutputSizeObservation final
	{
	public:
		void Publish(std::uint32_t a_width, std::uint32_t a_height) noexcept
		{
			if (a_width == 0 || a_height == 0) {
				return;
			}

			const auto packed = (static_cast<std::uint64_t>(a_width) << 32) | a_height;
			_packed.store(packed, std::memory_order_release);
		}

		std::optional<OutputSize> Snapshot() const noexcept
		{
			const auto packed = _packed.load(std::memory_order_acquire);
			if (packed == 0) {
				return std::nullopt;
			}

			return OutputSize{
				.width = static_cast<std::uint32_t>(packed >> 32),
				.height = static_cast<std::uint32_t>(packed),
			};
		}

	private:
		std::atomic<std::uint64_t> _packed{ 0 };
	};
}
